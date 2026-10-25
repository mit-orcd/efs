#include "efs/record_decode.h"
#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include "efs/rdma_message.h"
#include "efs/checksum.h"
#include "efs/store.h"
#include "efs/put_upload.h"
#include "efs/opid.h"
#include "efs/meta_cmd.h"
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include "efs/version.h"
#include "efs/storage_capacity.h"
#include "server_internal.h"
#include "dest_fault.h"
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

/* Optional op-id suffix on a directory RPC (protocol.h EFS_DIROP_OPID_LEN):
 * present iff the payload is exactly struct + suffix and names a real
 * identity. Anything else = no op-id (pre-I16 client). */
static const struct efs_opid_req *dirop_opid(const void *payload, uint32_t plen,
                                             uint32_t slen,
                                             struct efs_opid_req *q)
{
    if (plen != slen + EFS_OPID_WIRE_LEN)
        return NULL;
    efs_opid_req_unpack(q, (const uint8_t *)payload + slen);
    return efs_opid_req_valid(q) && q->id.session_epoch ? q : NULL;
}

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

/* A retired message ID (EFS_MSG_IS_RETIRED): this build has no handler, so
 * the sender learns it by a closed connection, never by silence. */
static int retired_refuse(uint8_t type)
{
    static uint64_t refused;
    uint64_t n = __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
    if (n <= 10 || !(n % 1000))
        fprintf(stderr, "handler: refused retired message %u; this build does not "
                        "serve it; closing (%llu so far)\n",
                type, (unsigned long long)n);
    return 1;
}

/* W153: client metadata before formation. The cluster has no committed
 * membership, so no count, no group-2 voters and no namespace: refuse it
 * like a retired message (closed connection, one bounded log line). */
static int unformed_metadata(uint8_t type)
{
    switch (type) {
    case EFS_MSG_INODE_LOOKUP: case EFS_MSG_INODE_CREATE: case EFS_MSG_INODE_GETATTR:
    case EFS_MSG_INODE_READDIR: case EFS_MSG_INODE_UNLINK: case EFS_MSG_INODE_SETATTR:
    case EFS_MSG_INODE_LINK: case EFS_MSG_REPORT_CHUNKS: case EFS_MSG_INODE_GETCHUNKS:
    case EFS_MSG_INODE_APPEND: case EFS_MSG_INODE_LOOKUP_PATH: case EFS_MSG_INODE_HOLD:
    case EFS_MSG_INODE_FLOCK: case EFS_MSG_INODE_RENAME_AT: case EFS_MSG_XATTR:
    case EFS_MSG_INODE_WRITER_VIEW: case EFS_MSG_LANE_WRITER_VIEW: case EFS_MSG_LANE_BOOTSTRAP:
    case EFS_MSG_PUBLICATION: case EFS_MSG_PUBLICATION_STATUS: case EFS_MSG_PUBLICATION_RETIRE:
    case EFS_MSG_QUERY_SNAPSHOT: case EFS_MSG_CREATE_RETAINED:
        break;
    default:
        return 0;
    }
    if (server_raft_host_formed())
        return 0;
    static uint64_t refused;
    uint64_t n = __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
    if (n <= 10 || !(n % 1000))
        fprintf(stderr, "handler: refused metadata message %u: the cluster is not formed; "
                        "start at least three members and run efs-mgmt raft-mkfs once; "
                        "closing (%llu so far)\n", type, (unsigned long long)n);
    return 1;
}

/* K1/W145 part 4: after a stop request a handler finishes the request in
 * hand, then closes its connection instead of reading the next one. Pooled
 * RDMA clients kept one handler alive for minutes by retrying a group read
 * that could no longer get a quorum (the TCP SHUT_RD wake never reaches the
 * RDMA channel). */
static int g_handlers_stopping;
static uint64_t g_handler_stop_closes;

void server_handlers_begin_stop(void)
{
    __atomic_store_n(&g_handlers_stopping, 1, __ATOMIC_RELEASE);
}

int server_handlers_stopping(void)
{
    return __atomic_load_n(&g_handlers_stopping, __ATOMIC_ACQUIRE);
}

static int handler_stop_gate(void)
{
    if (!__atomic_load_n(&g_handlers_stopping, __ATOMIC_ACQUIRE))
        return 0;
    uint64_t n = __atomic_add_fetch(&g_handler_stop_closes, 1, __ATOMIC_RELAXED);
    if (n == 1 || !(n % 256))
        fprintf(stderr, "handler: shutdown: closing connection before the next request "
                "(closed=%llu)\n", (unsigned long long)n);
    return 1;
}

void server_handle_conn(struct efs_conn *conn)
{
    int fd = conn->fd;
    struct efs_decl_gate decl = {0}; /* W133: one declaration per connection */
    int close_conn = 0;
    while (!close_conn) {
        uint8_t type = 0;
        void *payload = NULL;
        uint32_t payload_len = 0;
        void *to_free = NULL;
        int rdma_frame = 0;
        int rc = 0;

        if (handler_stop_gate())
            break;
        int chan = efs_conn_wait_request(conn);
        if (chan < 0)
            break;
        conn->recv_chan = chan;
        if (efs_rdma_trace_on()) {
            static int nwait;
            int n = __sync_fetch_and_add(&nwait, 1);
            if (n < 8)
                fprintf(stderr, "rdma-trace: wait_request chan=%s rc=%p\n",
                        chan == EFS_CONN_RDMA ? "RDMA" : "TCP",
                        (void *)conn->rc);
        }

        if (chan == EFS_CONN_RDMA) {
            /* Complete assembled request remains owned until the synchronous
             * switch finishes; physical segment buffers are already reposted. */
            int wr = efs_rdma_message_wait(conn->rc, -1);
            if (wr == EFS_ERR_AGAIN)
                continue; /* TCP side-channel has a request */
            if (wr != 0)
                break;
            uint32_t flen = 0;
            uint8_t *frame = efs_rdma_message_frame(conn->rc, &flen);
            if (!frame || flen < 5) {
                efs_rdma_message_release(conn->rc);
                break;
            }
            uint32_t nlen;
            memcpy(&nlen, frame, 4);
            nlen = ntohl(nlen);
            if (nlen == 0 || nlen > efs_rdma_max_frame(conn->rc)-4 || flen != 4 + nlen) {
                efs_rdma_message_release(conn->rc);
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

        /* W133: nothing but the declaration itself and HEARTBEAT is served
         * before an accepted CLIENT_HELLO; the gate answers declarations
         * and refuses everything else fail-closed (no reply, connection
         * closed, one logged line). Retired IDs are refused as retired. */
        int gate = EFS_GATE_DISPATCH;
        if (!EFS_MSG_IS_RETIRED(type))
            gate = efs_decl_gate_frame(&decl, conn, type, payload, payload_len);
        if (gate == EFS_GATE_REFUSE)
            close_conn = 1;
        if (EFS_MSG_IS_RETIRED(type))
            close_conn = retired_refuse(type);
        else if (gate == EFS_GATE_DISPATCH && unformed_metadata(type))
            close_conn = 1;
        /* A refused frame gets no reply. (Rewriting its type to 0 used to
         * dispatch it as HEARTBEAT and answer a HEARTBEAT_ACK first.) */
        if (close_conn) {
            if (rdma_frame)
                efs_rdma_message_release(conn->rc);
            free(to_free);
            break;
        }
        if (gate == EFS_GATE_ANSWERED) {
            /* The declaration was answered by the gate; nothing to dispatch. */
            if (rdma_frame)
                efs_rdma_message_release(conn->rc);
            free(to_free);
            continue;
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
                struct efs_msg_hello decoded = {0};
                struct efs_msg_hello *h = &decoded;
                int version_ok = efs_peer_hello_decode(payload, payload_len, h) == EFS_OK;
                /* Diagnostics never dereference a short/unaligned input or
                 * print an unterminated peer-provided field. */
                if (!version_ok && payload)
                    memcpy(h, payload, payload_len < sizeof(*h) ? payload_len : sizeof(*h));
                h->addr[sizeof(h->addr)-1] = 0;
                h->build_id[sizeof(h->build_id)-1] = 0;
                if (!version_ok) {
                    /* Heap: hello_ack embeds nodes[EFS_MAX_NODES] (~68 KiB at 16). */
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
                /* K33 (M4', M5): a joining store (no node id) never enters
                 * the node table: it is told to wait, to promise the seed's
                 * formation proposal, or given the committed revision that
                 * names it, or refused. */
                if (h->node_id == 0) {
                    char haddr[sizeof(h->addr)];
                    memcpy(haddr, h->addr, sizeof(haddr));
                    haddr[sizeof(haddr) - 1] = '\0';
                    struct efs_msg_hello_ack *ja = calloc(1, sizeof(*ja));
                    if (!ja)
                        break;
                    strncpy(ja->build_id, EFS_BUILD_ID, sizeof(ja->build_id) - 1);
                    server_member_hello_join(haddr, h->port, h->token, ja);
                    efs_conn_send_msg(conn, EFS_MSG_HELLO_ACK, ja, sizeof(*ja));
                    free(ja);
                    break;
                }
                /* W153/K33 (M5): a formed cluster admits its committed members
                 * at their committed endpoint and store; anything else is
                 * refused and the sender exits. Before formation the seed's
                 * and the promised stores of its cluster form the table. */
                {
                    char haddr[sizeof(h->addr)];
                    uint32_t committed = 0;
                    memcpy(haddr, h->addr, sizeof(haddr));
                    haddr[sizeof(haddr) - 1] = '\0';
                    uint32_t why = server_member_hello(h->node_id, haddr, h->port, h->token,
                                                       h->cluster, h->flags, &committed);
                    if (why != EFS_HELLO_REJECT_NONE) {
                        struct efs_msg_hello_ack *rej = calloc(1, sizeof(*rej));
                        if (!rej)
                            break;
                        rej->reject_reason = why;
                        rej->node_count = committed;
                        strncpy(rej->build_id, EFS_BUILD_ID, sizeof(rej->build_id) - 1);
                        if (why == EFS_HELLO_REJECT_FORMED)
                            fprintf(stderr, "HELLO refused: node %u (%s:%u, store %016llx) cannot "
                                    "be a member of this cluster (%u members): its id or "
                                    "endpoint belongs to another store, or it belongs to "
                                    "another cluster\n", h->node_id, haddr, h->port,
                                    (unsigned long long)h->token, committed);
                        efs_conn_send_msg(conn, EFS_MSG_HELLO_ACK, rej, sizeof(*rej));
                        free(rej);
                        break;
                    }
                }
                pthread_mutex_lock(&g_server->lock);

                /* Update existing entry or append a new one; never duplicate. */
                uint32_t idx = g_server->node_count;
                int changed = 0;
                int accepted = 0;
                int dup = 0;
                for (uint32_t i = 0; i < g_server->node_count; i++) {
                    if (g_server->nodes[i].id == h->node_id) {
                        idx = i;
                        break;
                    }
                }
                if (idx < g_server->node_count && g_server->nodes[idx].token &&
                    g_server->nodes[idx].token != h->token) {
                    /* K33 (M2): before formation one store per node id. */
                    fprintf(stderr, "HELLO refused: node id %u is held by store %016llx at "
                            "%s:%u; %s:%u announces store %016llx\n", h->node_id,
                            (unsigned long long)g_server->nodes[idx].token,
                            g_server->nodes[idx].addr, g_server->nodes[idx].port, h->addr,
                            h->port, (unsigned long long)h->token);
                    idx = EFS_MAX_NODES + 1; /* neither refreshed nor appended */
                    dup = 1;
                } else if (idx < g_server->node_count) {
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
                    if (n->token != h->token)
                        changed = 1;
                    n->token = h->token;
                    accepted = 1;
                } else if (idx == g_server->node_count && g_server->node_count < EFS_MAX_NODES) {
                    struct efs_node *n = &g_server->nodes[g_server->node_count];
                    n->id = h->node_id;
                    strncpy(n->addr, h->addr, sizeof(n->addr) - 1);
                    n->port = h->port;
                    strncpy(n->storage_path, h->storage_path,
                            sizeof(n->storage_path) - 1);
                    n->quota = h->quota;
                    n->used = h->used;
                    n->token = h->token;
                    g_server->node_count++;
                    changed = 1;
                    accepted = 1;
                }

                server_dedupe_nodes_locked(g_server);
                server_heartbeat_apply_locked(g_server);
                /* Heap: hello_ack embeds nodes[EFS_MAX_NODES] (~68 KiB at 16). */
                struct efs_msg_hello_ack *ack = calloc(1, sizeof(*ack));
                if (!ack) {
                    pthread_mutex_unlock(&g_server->lock);
                    break;
                }
                ack->epoch = g_server->epoch;
                /* assigned_id == 0 means reject (e.g. cluster at capacity). */
                ack->assigned_id = accepted ? h->node_id : 0;
                ack->reject_reason = accepted ? EFS_HELLO_REJECT_NONE
                                     : dup ? EFS_HELLO_REJECT_FORMED : EFS_HELLO_REJECT_FULL;
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
                uint64_t io_t0 = efs_iostats_now_us();
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex =
                    server_export_acquire_or_create_locked(g_server,
                                                          req->export_id);
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
                            efs_iostats_add(EFS_IOSTAT_GET, 0,
                                            efs_iostats_now_us() - io_t0, 1);
                            server_export_put(g_server, ex);
                            break;
                        }
                        tls_reply = nb;
                        tls_reply_cap = need;
                    }
                    reply = tls_reply;
                }
                uint32_t out_len = 1;
                if (!ex || server_test_dest_fault("get")) {
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
                        .chunk_generation = req->chunk_generation,
                    };
                    data_len = server_frag_len(ex, req->ino);
                    efs_store_nvme_bind(&st, &nctx, g_server, ex);
                    rc = efs_store_get(&st, &fid, dptr, &data_len,
                                       reply + 1, &sum_ok);
                    /* I25: the store verified the fragment record (format,
                     * identity = this request's export/ino/ci/fi/generation,
                     * length) and, for data fragments, the payload hash
                     * before returning OK; reply+1 is the payload digest.
                     * A refused fragment is reported as INTEGRITY, never
                     * served; repair is a control-plane job (W74). */
                    if (rc == EFS_OK && sum_ok) {
                        reply[0] = EFS_GET_CHUNK_OK;
                        out_len = 1 + EFS_HASH_SIZE + data_len;
                    } else if (rc == EFS_ERR_FRAG_FORMAT ||
                               rc == EFS_ERR_FRAG_IDENTITY ||
                               rc == EFS_ERR_CHECKSUM) {
                        reply[0] = EFS_GET_CHUNK_INTEGRITY;
                    } else {
                        reply[0] = EFS_GET_CHUNK_NOT_FOUND;
                    }
                }

                if (rdma_buf)
                    efs_rdma_send_commit(conn->rc, reply,
                                         EFS_MSG_GET_CHUNK_REPLY, out_len);
                else
                    efs_conn_send_msg(conn, EFS_MSG_GET_CHUNK_REPLY, reply,
                                      out_len);
                efs_iostats_add(EFS_IOSTAT_GET,
                                reply[0] == EFS_GET_CHUNK_OK
                                    ? out_len - 1 - EFS_HASH_SIZE
                                    : 0,
                                efs_iostats_now_us() - io_t0,
                                reply[0] != EFS_GET_CHUNK_OK);
                server_export_put(g_server, ex);
            }
            break;
        }
        case EFS_MSG_PUT_CHUNK: {
            /* K13: the one data PUT. The header names an ADMITTED ticket;
             * server_put_upload validates it, the session storage fence and
             * the exact fragment bytes before storing. Nothing else stores. */
            uint64_t start=efs_iostats_now_us();
            uint8_t reply[EFS_PUT_REPLY_LEN],path=0xff;
            int result=server_put_upload(g_server,payload,payload_len,&path);
            efs_put_reply_encode(result,path,g_server->put_boot_epoch,reply);
            efs_conn_send_msg(conn,EFS_MSG_PUT_CHUNK_REPLY,reply,sizeof(reply));
            efs_iostats_add(EFS_IOSTAT_PUT,result==EFS_OK?payload_len-EFS_PUT_UPLOAD_HEADER_LEN:0,
                efs_iostats_now_us()-start,result!=EFS_OK);
            if(result==EFS_OK)server_fault_crash_point("put-acked");
            break;
        }
        case EFS_MSG_REBUILD_PUT: {
            /* W154 rebuild of a published object's missing fragment: the
             * durable degraded mark, not a ticket, authorizes it (K13). */
            uint8_t reply[EFS_PUT_REPLY_LEN],path=0xff;
            int result=server_test_dest_fault("rebuild");
            if(result==EFS_OK)result=server_rebuild_put(g_server,payload,payload_len,&path);
            efs_put_reply_encode(result,path,g_server->put_boot_epoch,reply);
            efs_conn_send_msg(conn,EFS_MSG_REBUILD_PUT_REPLY,reply,sizeof(reply));
            break;
        }
        case EFS_MSG_PUT_ORPHAN_DELETE: {
            /* D31: one abandoned fragment, deleted only behind this
             * member's storage floor and the group's orphan record. */
            struct efs_msg_put_orphan_delete_reply reply = {
                .rc = server_put_orphan_delete(g_server, payload, payload_len)};
            efs_conn_send_msg(conn, EFS_MSG_PUT_ORPHAN_DELETE_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_GC_INODE_BATCH: {
            /* W124: journal-first acceptance of a reaped-inode batch. Each
             * rc is OK only once the record is fsynced and its fence set. */
            uint64_t start = efs_iostats_now_us();
            const struct efs_msg_gc_inode_batch *req=payload;
            uint32_t cnt = payload_len >= sizeof(*req) ? req->count : 0;
            int ok = payload_len >= sizeof(*req) && cnt && cnt <= EFS_GC_INODE_BATCH_MAX &&
                     payload_len == sizeof(*req) + (size_t)cnt * 16u;
            size_t rlen = sizeof(struct efs_msg_gc_inode_batch_reply) + (size_t)(ok ? cnt : 0) * 4u;
            struct efs_msg_gc_inode_batch_reply *rep = calloc(1, rlen);
            struct efs_reclaim_item *items = ok ? calloc(cnt, sizeof(*items)) : NULL;
            if (!rep || (ok && !items)) {
                free(rep); free(items);
                struct efs_msg_gc_inode_batch_reply e={0};
                efs_conn_send_msg(conn,EFS_MSG_GC_INODE_BATCH_REPLY,&e,sizeof(e));
                break;
            }
            if (ok) {
                rep->count = cnt;
                for (uint32_t i = 0; i < cnt; i++) {
                    items[i] = (struct efs_reclaim_item){.export_id=req->export_id,
                        .ino=req->items[i].ino,.generation=req->items[i].generation};
                    rep->rc[i] = EFS_ERR_BUSY;
                }
                (void)server_reclaim_accept(g_server, items, cnt, rep->rc);
            }
            efs_iostats_add(EFS_IOSTAT_GC_DELETE, 0, efs_iostats_now_us() - start, !ok);
            efs_conn_send_msg(conn,EFS_MSG_GC_INODE_BATCH_REPLY,rep,(uint32_t)rlen);
            free(items);
            free(rep);
            break;
        }
        case EFS_MSG_PUT_SEGMENT: {
            /* W125: one packed segment fragment from the group's packer. */
            struct efs_msg_segment_put_reply rep = {.rc = server_segment_put(g_server, payload, payload_len)};
            efs_conn_send_msg(conn, EFS_MSG_PUT_SEGMENT_REPLY, &rep, sizeof(rep));
            break;
        }
        case EFS_MSG_SEGMENT_AUDIT: {
            /* K37: verify the named segment fragments placed on this node. */
            uint8_t *reply = NULL;
            uint32_t rlen = 0;
            server_segment_audit(g_server, payload, payload_len, &reply, &rlen);
            if (!reply) { close_conn = 1; break; }
            efs_conn_send_msg(conn, EFS_MSG_SEGMENT_AUDIT_REPLY, reply, rlen);
            free(reply);
            break;
        }
        case EFS_MSG_GET_SEGMENT: {
            uint8_t *reply = NULL;
            uint32_t rlen = 0;
            if (server_segment_get(g_server, payload, payload_len, &reply, &rlen) != EFS_OK || !reply) {
                uint8_t e[EFS_SEGMENT_GET_REPLY] = {0};
                uint32_t v = (uint32_t)EFS_ERR_INVAL;
                e[0] = (uint8_t)(v >> 24); e[1] = (uint8_t)(v >> 16); e[2] = (uint8_t)(v >> 8); e[3] = (uint8_t)v;
                efs_conn_send_msg(conn, EFS_MSG_GET_SEGMENT_REPLY, e, sizeof(e));
                free(reply);
                break;
            }
            efs_conn_send_msg(conn, EFS_MSG_GET_SEGMENT_REPLY, reply, rlen);
            free(reply);
            break;
        }
        case EFS_MSG_GC_SEGMENT_BATCH: {
            /* W125: journal-first acceptance of retired segments. Each rc is
             * OK only once the record is fsynced and the fence installed. */
            const uint8_t *q = payload;
            uint32_t cnt = payload_len >= 8 ? ((uint32_t)q[4] << 24 | (uint32_t)q[5] << 16 |
                                               (uint32_t)q[6] << 8 | q[7]) : 0;
            int ok = payload_len >= 8 && cnt && cnt <= EFS_GC_SEGMENT_BATCH_MAX &&
                     payload_len == 8u + (size_t)cnt * 16u;
            size_t rlen = sizeof(struct efs_msg_gc_inode_batch_reply) + (size_t)(ok ? cnt : 0) * 4u;
            struct efs_msg_gc_inode_batch_reply *rep = calloc(1, rlen);
            struct efs_reclaim_item *items = ok ? calloc(cnt, sizeof(*items)) : NULL;
            if (!rep || (ok && !items)) {
                free(rep); free(items);
                struct efs_msg_gc_inode_batch_reply e = {0};
                efs_conn_send_msg(conn, EFS_MSG_GC_SEGMENT_BATCH_REPLY, &e, sizeof(e));
                break;
            }
            if (ok) {
                rep->count = cnt;
                for (uint32_t i = 0; i < cnt; i++) {
                    uint64_t id = 0, gen = 0;
                    for (int b = 0; b < 8; b++) {
                        id = id << 8 | q[8 + 16 * i + b];
                        gen = gen << 8 | q[16 + 16 * i + b];
                    }
                    items[i] = (struct efs_reclaim_item){.export_id = EFS_RECLAIM_SEGMENT_EID(q[0]),
                        .ino = id, .generation = gen, .kind = EFS_RECLAIM_SEGMENT};
                    rep->rc[i] = EFS_ERR_BUSY;
                }
                (void)server_reclaim_accept(g_server, items, cnt, rep->rc);
            }
            efs_conn_send_msg(conn, EFS_MSG_GC_SEGMENT_BATCH_REPLY, rep, (uint32_t)rlen);
            free(items);
            free(rep);
            break;
        }
        case EFS_MSG_GC_FRAG_BATCH: {
            /* W125: journal-first acceptance of superseded fragments; rc OK
             * once fsynced in the journal (deletion follows, checksum-guarded). */
            const uint8_t *q = payload;
            uint32_t eid = 0, cnt = 0;
            if (payload_len >= 8) {
                for (int b = 0; b < 4; b++) { eid = eid << 8 | q[b]; cnt = cnt << 8 | q[4 + b]; }
            }
            int ok = payload_len >= 8 && eid && cnt && cnt <= EFS_GC_FRAG_BATCH_MAX &&
                     payload_len == 8u + (size_t)cnt * EFS_GC_FRAG_ITEM_BYTES;
            size_t rlen = sizeof(struct efs_msg_gc_inode_batch_reply) + (size_t)(ok ? cnt : 0) * 4u;
            struct efs_msg_gc_inode_batch_reply *rep = calloc(1, rlen);
            struct efs_reclaim_item *items = ok ? calloc(cnt, sizeof(*items)) : NULL;
            if (!rep || (ok && !items)) {
                free(rep); free(items);
                struct efs_msg_gc_inode_batch_reply e = {0};
                efs_conn_send_msg(conn, EFS_MSG_GC_FRAG_BATCH_REPLY, &e, sizeof(e));
                break;
            }
            if (ok) {
                rep->count = cnt;
                for (uint32_t i = 0; i < cnt; i++) {
                    const uint8_t *m = q + 8 + (size_t)i * EFS_GC_FRAG_ITEM_BYTES;
                    uint64_t ino = 0, gen = 0;
                    uint32_t ci = 0;
                    for (int b = 0; b < 8; b++) { ino = ino << 8 | m[b]; gen = gen << 8 | m[8 + b]; }
                    for (int b = 0; b < 4; b++) ci = ci << 8 | m[16 + b];
                    items[i] = (struct efs_reclaim_item){.export_id = eid, .ino = ino, .generation = gen,
                        .kind = EFS_RECLAIM_SUPERSEDED, .chunk = ci, .fragment = m[20]};
                    memcpy(items[i].sum, m + 24, EFS_HASH_SIZE);
                    rep->rc[i] = EFS_ERR_BUSY;
                }
                (void)server_reclaim_accept(g_server, items, cnt, rep->rc);
            }
            efs_conn_send_msg(conn, EFS_MSG_GC_FRAG_BATCH_REPLY, rep, (uint32_t)rlen);
            free(items);
            free(rep);
            break;
        }
        case EFS_MSG_STORAGE_SESSION_FENCE: {
            struct efs_msg_storage_session_fence_reply reply={.rc=EFS_ERR_INVAL};
            if(payload_len==sizeof(struct efs_msg_storage_session_fence))
                server_raft_host_storage_session_fence(payload,&reply);
            efs_conn_send_msg(conn,EFS_MSG_STORAGE_SESSION_FENCE_REPLY,&reply,sizeof(reply));
            break;
        }
        case EFS_MSG_PACK_OBJECT_READ: {
            struct efs_msg_pack_object_read_reply *rep = calloc(1, sizeof(*rep));
            if (!rep) { close_conn = 1; break; }
            rep->rc = EFS_ERR_INVAL;
            if (payload_len == sizeof(struct efs_msg_pack_object_read))
                server_raft_host_pack_object_serve(payload, rep);
            efs_conn_send_msg(conn, EFS_MSG_PACK_OBJECT_READ_REPLY, rep, sizeof(*rep));
            free(rep);
            break;
        }
        case EFS_MSG_RECOVERY_SCAN: {
            /* K36: a recovering member's inventory page of a group it does
             * not host; answered only by a hosting member, never forwarded. */
            uint8_t *rep = NULL;
            uint32_t rlen = 0;
            server_raft_host_recovery_scan_serve(payload, payload_len, &rep, &rlen);
            if (!rep) { close_conn = 1; break; }
            efs_conn_send_msg(conn, EFS_MSG_RECOVERY_SCAN_REPLY, rep, rlen);
            free(rep);
            break;
        }
        case EFS_MSG_PACK_COMPACT: {
            struct efs_msg_pack_compact_reply rep = {.rc = EFS_ERR_INVAL};
            if (payload_len == sizeof(struct efs_msg_pack_compact))
                server_packer_compact_request(payload, &rep);
            efs_conn_send_msg(conn, EFS_MSG_PACK_COMPACT_REPLY, &rep, sizeof(rep));
            break;
        }
        case EFS_MSG_GC_STATUS: {
            struct efs_msg_gc_status_reply rep;
            server_gc_status(g_server, &rep);
            efs_conn_send_msg(conn, EFS_MSG_GC_STATUS_REPLY, &rep, sizeof(rep));
            break;
        }
        case EFS_MSG_XATTR: {
            struct efs_msg_xattr_reply rep;
            uint32_t rlen = (uint32_t)offsetof(struct efs_msg_xattr_reply, data);
            struct efs_msg_xattr *req;
            const uint8_t *name, *val;
            uint32_t body;

            memset(&rep, 0, rlen);
            rep.status = EFS_INODE_RPC_INVAL;
            if (server_raft_host_active() &&
                payload_len >= sizeof(struct efs_msg_xattr)) {
                req = payload;
                body = (uint32_t)req->nlen + req->vlen;
                /* SET/REMOVE carry the mount session suffix (K5). */
                uint32_t sess = (req->op == EFS_XATTR_SET ||
                                 req->op == EFS_XATTR_REMOVE) ? EFS_SESS_WIRE_LEN : 0;
                if (req->nlen <= EFS_XATTR_NAME_MAX &&
                    req->vlen <= EFS_XATTR_VALUE_MAX &&
                    payload_len == sizeof(*req) + body + sess) {
                    const uint8_t *su = NULL;
                    uint32_t se = 0;
                    name = (const uint8_t *)payload + sizeof(*req);
                    val = name + req->nlen;
                    if (sess) {
                        su = val + req->vlen;
                        memcpy(&se, su + EFS_OPID_UUID_LEN, 4);
                    }
                    server_raft_host_xattr(req->ino, req->op, req->flags, name,
                                           req->nlen, val, req->vlen, su, se,
                                           &rep, &rlen);
                } else if (sess && req->nlen <= EFS_XATTR_NAME_MAX &&
                           req->vlen <= EFS_XATTR_VALUE_MAX &&
                           payload_len == sizeof(*req) + body) {
                    rep.status = EFS_INODE_RPC_UNREGISTERED;
                }
            }
            efs_conn_send_msg(conn, EFS_MSG_XATTR_REPLY, &rep, rlen);
            break;
        }
        case EFS_MSG_BENCH_PUT: {
            /* Network bench: accept mount-shaped PUT payload, ACK, discard. */
            uint8_t reply = EFS_BENCH_PUT_ERROR;
            if (payload_len >= sizeof(struct efs_msg_bench_put)) {
                struct efs_msg_bench_put *req = payload;
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
            memcpy(reply->nodes, g_server->nodes, sizeof(g_server->nodes));
            pthread_mutex_unlock(&g_server->lock);
            /* W153: clients place data over the committed members only. */
            reply->node_count = server_member_filter(reply->nodes, node_count);
            efs_conn_send_msg(conn, EFS_MSG_LIST_NODES_REPLY, reply, sizeof(*reply));
            free(reply);
            break;
        }
        case EFS_MSG_MEMBERSHIP_READ: {
            /* K33 live client view: the committed record this node holds
             * (never the HELLO table). The client judges and installs it. */
            struct efs_msg_membership_reply *mr = calloc(1, sizeof(*mr));
            struct efs_membership mb;
            size_t mlen = 0;
            if (!mr)
                break;
            if (!server_membership_get(&mb))
                mr->rc = EFS_ERR_UNFORMED;
            else if (efs_membership_encode(&mb, mr->record, sizeof(mr->record), &mlen) != EFS_OK)
                mr->rc = EFS_ERR_INVAL;
            else
                mr->len = (uint32_t)mlen;
            efs_conn_send_msg(conn, EFS_MSG_MEMBERSHIP_READ_REPLY, mr,
                              EFS_MEMBERSHIP_REPLY_HDR + mr->len);
            free(mr);
            break;
        }
        case EFS_MSG_STATUS: {
            pthread_mutex_lock(&g_server->lock);
            struct efs_msg_status_reply reply;
            memset(&reply, 0, sizeof(reply));
            reply.quota = g_server->quota;
            struct efs_node *local = server_local_node(g_server);
            reply.used = local ? local->used : 0;
            /* No drain/leave state machine; always ACTIVE. */
            reply.state = 0;
            pthread_mutex_unlock(&g_server->lock);
            reply.capacity_valid = efs_storage_capacity(
                g_server->storage_paths, g_server->storage_path_count,
                reply.quota, reply.used, &reply.capacity_bytes,
                &reply.available_bytes) == 0;
            for (int g = 0; g < 2; g++)
                reply.degraded_chunks[g] =
                    __atomic_load_n(&g_server->degraded_chunks[g], __ATOMIC_RELAXED);
            reply.degraded_sampled =
                __atomic_load_n(&g_server->degraded_sampled_mask, __ATOMIC_RELAXED);
            reply.rebuilt_fragments =
                __atomic_load_n(&g_server->rebuilt_fragments, __ATOMIC_RELAXED);
            reply.rebuild_failures =
                __atomic_load_n(&g_server->rebuild_failures, __ATOMIC_RELAXED);
            reply.rebuild_retired =
                __atomic_load_n(&g_server->rebuild_retired, __ATOMIC_RELAXED);
            for (int g = 0; g < 2; g++)
                reply.put_orphan_pending[g] =
                    __atomic_load_n(&g_server->put_orphan_pending[g], __ATOMIC_RELAXED);
            reply.put_orphan_sampled =
                __atomic_load_n(&g_server->put_orphan_sampled_mask, __ATOMIC_RELAXED);
            reply.put_orphan_collected =
                __atomic_load_n(&g_server->put_orphan_collected, __ATOMIC_RELAXED);
            reply.put_orphan_deleted =
                __atomic_load_n(&g_server->put_orphan_deleted, __ATOMIC_RELAXED);
            reply.put_orphan_failures =
                __atomic_load_n(&g_server->put_orphan_failures, __ATOMIC_RELAXED);
            for (int g = 0; g < 2; g++) {
                reply.degraded_segments[g] =
                    __atomic_load_n(&g_server->degraded_segments[g], __ATOMIC_RELAXED);
                reply.unrecoverable_segments[g] =
                    __atomic_load_n(&g_server->unrecoverable_segments[g], __ATOMIC_RELAXED);
            }
            reply.segment_sampled =
                __atomic_load_n(&g_server->segment_sampled_mask, __ATOMIC_RELAXED);
            /* K38: for the groups this node leads (the sampled bits), the
             * degraded counts come from the local replica at this reply,
             * not from the last rebuild pass: a stripe or segment marked
             * degraded after the pass sampled was reported as "Protection:
             * full" for up to a GC pass, and a member stopped on that word
             * took the stripe's second fragment (test_k33_payload.py step 5
             * read EIO). The chunk count is the apply-kept one (W154; it
             * may overshoot, never undershoot); segments are counted now. */
            for (int g = 0; g < 2; g++) {
                uint8_t group = g ? EFS_RAFT_GROUP_SHARD2 : EFS_RAFT_GROUP_SHARD;
                if (reply.degraded_sampled & (1u << g)) {
                    int64_t have = efs_meta_degraded_count_peek(g ? 2u : 1u);
                    if (have >= 0)
                        reply.degraded_chunks[g] = (uint64_t)have;
                }
                uint64_t total = 0, lost = 0;
                if ((reply.segment_sampled & (1u << g)) &&
                    server_segment_degraded_now(group, &total, &lost) == EFS_OK) {
                    reply.degraded_segments[g] = total;
                    reply.unrecoverable_segments[g] = lost;
                }
            }
            reply.segments_rebuilt =
                __atomic_load_n(&g_server->segments_rebuilt, __ATOMIC_RELAXED);
            reply.segment_rebuild_failures =
                __atomic_load_n(&g_server->segment_rebuild_failures, __ATOMIC_RELAXED);
            reply.segment_marks_retired =
                __atomic_load_n(&g_server->segment_marks_retired, __ATOMIC_RELAXED);
            reply.segment_audit_marked =
                __atomic_load_n(&g_server->segment_audit_marked, __ATOMIC_RELAXED);
            reply.segment_audit_checked =
                __atomic_load_n(&g_server->segment_audit_checked, __ATOMIC_RELAXED);
            efs_conn_send_msg(conn, EFS_MSG_STATUS_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_DATA_COMMIT: {
            /* NFS COMMIT: everything this server acknowledged before the
             * request is durable when the reply is OK. */
            struct efs_msg_data_commit_reply reply;
            memset(&reply, 0, sizeof(reply));
            /* K35 test hook: a member that dies during a client's COMMIT. */
            server_test_member_exit("data-commit");
            reply.rc = server_test_dest_fault("commit");
            if (reply.rc == EFS_OK)
                reply.rc = server_put_sync_commit(g_server, &reply.roots);
            reply.boot_epoch = g_server->put_boot_epoch;
            efs_conn_send_msg(conn, EFS_MSG_DATA_COMMIT_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_SYNC_STATS: {
            struct efs_msg_sync_stats_reply *reply = calloc(1, sizeof(*reply));
            if (!reply)
                break;
            server_put_sync_stats(g_server, reply);
            server_recovery_stats(&reply->recovery);
            efs_conn_send_msg(conn, EFS_MSG_SYNC_STATS_REPLY, reply, sizeof(*reply));
            free(reply);
            break;
        }
        case EFS_MSG_IO_STATS: {
            struct efs_msg_io_stats_reply reply;
            efs_iostats_snapshot(&reply);
            efs_conn_send_msg(conn, EFS_MSG_IO_STATS_REPLY, &reply,
                              sizeof(reply));
            break;
        }
        case EFS_MSG_VERSION: {
            struct efs_msg_version_reply reply;
            char ver[256];
            memset(&reply, 0, sizeof(reply));
            snprintf(reply.version, sizeof(reply.version), "%s",
                     efs_version_string("efsd", ver, sizeof(ver)));
            efs_conn_send_msg(conn, EFS_MSG_VERSION_REPLY, &reply,
                              sizeof(reply));
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
                    /* Nothing drains usage down to a new quota, so a
                     * shrink below current usage is refused outright. */
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
        case EFS_MSG_CREATE_RETAINED: {
            struct efs_msg_create_retained_reply reply;
            memset(&reply,0,sizeof(reply));reply.rpc.status=EFS_INODE_RPC_INVAL;
            if(payload_len==sizeof(struct efs_msg_create_retained) && server_raft_host_active())
                server_raft_host_create_retained(payload,&reply);
            efs_conn_send_msg(conn,EFS_MSG_CREATE_RETAINED_REPLY,&reply,sizeof(reply));
            break;
        }
        case EFS_MSG_INODE_LOOKUP:
        case EFS_MSG_INODE_CREATE:
        case EFS_MSG_INODE_GETATTR:
        case EFS_MSG_INODE_UNLINK:
        case EFS_MSG_INODE_RENAME_AT:
        case EFS_MSG_INODE_SETATTR:
        case EFS_MSG_INODE_APPEND:
        case EFS_MSG_INODE_LINK:
        case EFS_MSG_INODE_HOLD:
        case EFS_MSG_INODE_FLOCK: {
            if (server_raft_host_active()) {
                struct efs_msg_inode_reply r;
                uint8_t rtype;
                memset(&r, 0, sizeof(r));
                r.status = EFS_INODE_RPC_ERROR;
                /* K5: namespace mutations and SETATTR name the mount session
                 * with a full op-id suffix. Without one the request is
                 * refused before any host work; there is no unidentified
                 * mutation. HOLD, FLOCK and APPEND are refused by the host
                 * gate when their session suffix is absent. */
                size_t base = type == EFS_MSG_INODE_CREATE ? sizeof(struct efs_msg_inode_create) :
                              type == EFS_MSG_INODE_UNLINK ? sizeof(struct efs_msg_inode_unlink) :
                              type == EFS_MSG_INODE_LINK ? sizeof(struct efs_msg_inode_link) :
                              type == EFS_MSG_INODE_RENAME_AT ? sizeof(struct efs_msg_inode_rename_at) :
                              type == EFS_MSG_INODE_SETATTR ? sizeof(struct efs_msg_inode_setattr) : 0;
                struct efs_opid_req required;
                if (base && payload_len >= base &&
                    !dirop_opid(payload, payload_len, (uint32_t)base, &required)) {
                    r.status = EFS_INODE_RPC_UNREGISTERED;
                    rtype = type == EFS_MSG_INODE_CREATE ? EFS_MSG_INODE_CREATE_REPLY :
                            type == EFS_MSG_INODE_UNLINK ? EFS_MSG_INODE_UNLINK_REPLY :
                            type == EFS_MSG_INODE_LINK ? EFS_MSG_INODE_LINK_REPLY :
                            type == EFS_MSG_INODE_RENAME_AT ? EFS_MSG_INODE_RENAME_AT_REPLY :
                                                              EFS_MSG_INODE_SETATTR_REPLY;
                    efs_conn_send_msg(conn, rtype, &r, sizeof(r));
                    break;
                }
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
                    struct efs_opid_req q;

                    server_raft_host_create(req->parent, req->name, req->mode,
                                            req->uid, req->gid, req->flags,
                                            req->owner,
                                            dirop_opid(payload, payload_len,
                                                       sizeof(*req), &q),
                                            &r);
                    rtype = EFS_MSG_INODE_CREATE_REPLY;
                } else if (type == EFS_MSG_INODE_UNLINK &&
                           payload_len >= sizeof(struct efs_msg_inode_unlink)) {
                    struct efs_msg_inode_unlink *req = payload;
                    struct efs_opid_req q;

                    server_raft_host_unlink(req->parent, req->name, req->is_dir,
                                            dirop_opid(payload, payload_len,
                                                       sizeof(*req), &q),
                                            &r);
                    rtype = EFS_MSG_INODE_UNLINK_REPLY;
                } else if (type == EFS_MSG_INODE_SETATTR &&
                           payload_len >= sizeof(struct efs_msg_inode_setattr)) {
                    struct efs_msg_inode_setattr *req = payload;
                    struct efs_opid_req q;
                    server_raft_host_setattr(req->ino, req->mask, req->mode,
                                              req->uid, req->gid, req->size,
                                              req->mtime, req->mtime_nsec,
                                              req->atime, req->atime_nsec,
                                              dirop_opid(payload, payload_len,
                                                         sizeof(*req), &q),
                                              &r);
                    rtype = EFS_MSG_INODE_SETATTR_REPLY;
                } else if (type == EFS_MSG_INODE_APPEND &&
                           payload_len >= sizeof(struct efs_msg_inode_append)) {
                    struct efs_msg_inode_append *req = payload;
                    const uint8_t *su = NULL;
                    uint32_t se = 0;

                    rtype = EFS_MSG_INODE_APPEND_REPLY;
                    if (payload_len == sizeof(*req) + EFS_APPEND_ACK_LEN ||
                        payload_len == sizeof(*req) + EFS_APPEND_OPID_LEN ||
                        payload_len == sizeof(*req) + EFS_SESS_WIRE_LEN) {
                        uint64_t seq = 0, ack = 0;
                        const uint64_t *ap = NULL;

                        su = (const uint8_t *)req + sizeof(*req);
                        memcpy(&se, su + 16, 4);
                        if (payload_len >= sizeof(*req) + EFS_APPEND_OPID_LEN)
                            memcpy(&seq, su + EFS_SESS_WIRE_LEN, 8);
                        if (payload_len == sizeof(*req) + EFS_APPEND_ACK_LEN) {
                            memcpy(&ack, su + EFS_APPEND_OPID_LEN, 8); ap = &ack;
                        }
                        server_raft_host_append(req->ino, req->len, su, se, seq, ap,
                                                &r);
                    } else if (payload_len == sizeof(*req)) {
                        server_raft_host_append(req->ino, req->len, NULL, 0, 0, NULL,
                                                &r);
                    } else {
                        r.status = EFS_INODE_RPC_INVAL;
                    }
                } else if (type == EFS_MSG_INODE_LINK &&
                           payload_len >= sizeof(struct efs_msg_inode_link)) {
                    struct efs_msg_inode_link *req = payload;
                    struct efs_opid_req q;

                    server_raft_host_link(req->src_ino, req->new_parent,
                                          req->new_name,
                                          dirop_opid(payload, payload_len,
                                                     sizeof(*req), &q),
                                          &r);
                    rtype = EFS_MSG_INODE_LINK_REPLY;
                } else if (type == EFS_MSG_INODE_RENAME_AT &&
                           payload_len >= sizeof(struct efs_msg_inode_rename_at)) {
                    struct efs_msg_inode_rename_at *req = payload;
                    struct efs_opid_req q;

                    server_raft_host_rename_at(req->old_parent, req->old_name,
                                               req->new_parent, req->new_name,
                                               dirop_opid(payload, payload_len,
                                                          sizeof(*req), &q),
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
            struct efs_opid session, *identity = NULL;
            struct efs_report_pubid pubid, *publication = NULL;
            uint32_t flags = 0;
            uint32_t count = 0;
            uint32_t ino_count = 0;
            struct efs_chunk_rec *recs = NULL;
            const struct efs_ino_size_rec *irecs = NULL;
            if (payload_len >= sizeof(struct efs_msg_report_chunks)) {
                struct efs_msg_report_chunks *req = payload;
                const uint8_t *raw = (const uint8_t *)payload + sizeof(*req);
                size_t avail = payload_len - sizeof(*req);
                flags = req->sync;
                if (flags & ~(EFS_REPORT_F_SYNC | EFS_REPORT_F_SESSION |
                              EFS_REPORT_F_PUBID | EFS_REPORT_F_RECEIPT)) bad = 1;
                if (flags & EFS_REPORT_F_SESSION) {
                    if (avail < EFS_REPORT_SESSION_LEN) bad = 1;
                    else {
                        avail -= EFS_REPORT_SESSION_LEN;
                        if (!efs_report_session_unpack(&session, raw + avail)) bad = 1;
                        else identity = &session;
                    }
                }
                if (!bad && (flags & EFS_REPORT_F_PUBID)) {
                    if (avail < EFS_REPORT_PUBID_LEN) bad = 1;
                    else {
                        avail -= EFS_REPORT_PUBID_LEN;
                        if (!efs_report_pubid_unpack(&pubid, raw + avail)) bad = 1;
                        else publication = &pubid;
                    }
                }
                size_t used = 0;
                ino_count = req->ino_count;
                count = req->count;
                if (bad || count > avail / offsetof(struct efs_chunk_rec, deltas))
                    bad = 1;
                else if (count > 0)
                    recs = calloc(count, sizeof(*recs));
                if (count && !recs)
                    bad = 1;
                else if (efs_chunk_recs_unpack(raw, avail, count, recs,
                                               &used) != 0)
                    bad = 1;
                else if (ino_count >
                         (avail - used) / sizeof(struct efs_ino_size_rec))
                    bad = 1;
                else if (avail - used != (size_t)ino_count * sizeof(*irecs))
                    bad = 1;
                else if (ino_count)
                    irecs = (const struct efs_ino_size_rec *)(raw + used);
            } else {
                bad = 1;
            }
            if (!bad && (flags & EFS_REPORT_F_RECEIPT)) {
                /* W155 receipt query: an identity and nothing to run. */
                struct efs_msg_report_receipt_reply rr;
                memset(&rr, 0, sizeof(rr));
                rr.rpc.status = EFS_INODE_RPC_ERROR;
                if (count || ino_count || !identity || !publication ||
                    (flags & ~(EFS_REPORT_F_SESSION | EFS_REPORT_F_PUBID |
                               EFS_REPORT_F_RECEIPT)))
                    rr.rpc.status = EFS_INODE_RPC_INVAL;
                else if (server_raft_host_active())
                    server_raft_host_report_receipt(identity, publication, &rr);
                efs_conn_send_msg(conn, EFS_MSG_REPORT_CHUNKS_REPLY, &rr, sizeof(rr));
                free(recs);
                break;
            }
            if (server_raft_host_active()) {
                if (bad)
                    r.status = EFS_INODE_RPC_INVAL;
                else
                    server_raft_host_report_receipted(recs, count, irecs, ino_count, flags,
                                                      identity, publication, &r);
            }
            efs_conn_send_msg(conn, EFS_MSG_REPORT_CHUNKS_REPLY, &r,
                              sizeof(r));
            free(recs);
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
        case EFS_MSG_PUBLICATION:
        case EFS_MSG_PUBLICATION_STATUS:
        case EFS_MSG_PUBLICATION_RETIRE: {
            struct efs_msg_publication_reply r = {0};
            r.rpc.status = EFS_INODE_RPC_INVAL;
            if (server_raft_host_active() && payload_len == sizeof(struct efs_msg_publication))
                server_raft_host_publication(payload, type == EFS_MSG_PUBLICATION_RETIRE ? 2 : type == EFS_MSG_PUBLICATION_STATUS, &r);
            efs_conn_send_msg(conn, type == EFS_MSG_PUBLICATION_RETIRE ? EFS_MSG_PUBLICATION_RETIRE_REPLY : type == EFS_MSG_PUBLICATION_STATUS ?
                EFS_MSG_PUBLICATION_STATUS_REPLY : EFS_MSG_PUBLICATION_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_LANE_BOOTSTRAP: {
            struct efs_msg_lane_writer_view_reply r = {0};
            r.view.status = EFS_INODE_RPC_INVAL;
            if (server_raft_host_active() && payload_len == sizeof(struct efs_msg_lane_bootstrap))
                server_raft_host_lane_bootstrap_rpc(payload, &r);
            efs_conn_send_msg(conn, EFS_MSG_LANE_BOOTSTRAP_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_LANE_WRITER_VIEW: {
            struct efs_msg_lane_writer_view_reply r = {0};
            r.view.status = EFS_INODE_RPC_INVAL;
            if (server_raft_host_active() && payload_len == sizeof(struct efs_msg_lane_writer_view))
                server_raft_host_lane_writer_view(payload, &r);
            efs_conn_send_msg(conn, EFS_MSG_LANE_WRITER_VIEW_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_INODE_WRITER_VIEW: {
            struct efs_msg_inode_writer_view_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_INVAL;
            if (server_raft_host_active() &&
                payload_len == sizeof(struct efs_msg_inode_writer_view))
                server_raft_host_writer_view(payload, &r);
            efs_conn_send_msg(conn, EFS_MSG_INODE_WRITER_VIEW_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_INODE_GETCHUNKS: {
            struct efs_msg_inode_getchunks_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            if (server_raft_host_active() &&
                payload_len >= sizeof(struct efs_msg_inode_getchunks)) {
                struct efs_msg_inode_getchunks *req = payload;
                server_raft_host_getchunks(req->export_id, req->ino, req->start, req->max, req->generation, &r);
            }
            if (r.count > EFS_GETCHUNKS_MAX) {
                r.status = EFS_INODE_RPC_ERROR; r.count = 0;
            }
            /* Header + count records, not the 98 KiB fixed struct: a
             * one-chunk lookup used to ship 63 empty records. */
            efs_conn_send_msg(conn, EFS_MSG_INODE_GETCHUNKS_REPLY, &r,
                              efs_getchunks_reply_len(r.count));
            break;
        }
        case EFS_MSG_RDMA_SETUP: {
            /* Bootstrap is valid only on a not-yet-upgraded TCP connection.
             * An application envelope cannot reopen setup or emit raw TCP. */
            if (chan != EFS_CONN_TCP || conn->rc) {
                if (rdma_frame) efs_rdma_message_release(conn->rc);
                free(to_free);
                return;
            }
            /* Arrived over TCP (the QP does not exist yet); the reply goes
             * back over TCP because recv_chan is TCP for this frame. */
            uint8_t rep[EFS_RMSG_SETUP_BYTES];
            uint32_t rlen = sizeof(rep);
            efs_rdma_server_accept(conn, payload, payload_len, &rep, &rlen);
            if (efs_send_msg(conn->fd, EFS_MSG_RDMA_SETUP_REPLY, rep, rlen)) {
                free(to_free); return;
            }
            if (efs_rdma_trace_on()) {
                /* Tag the peer's port: a QP and the TCP socket that carried
                 * its handshake must belong to the same connection, and that
                 * is exactly what a crossed SETUP reply would break. */
                struct sockaddr_in pa;
                socklen_t pl = sizeof(pa);
                unsigned pport = 0;
                if (getpeername(conn->fd, (struct sockaddr *)&pa, &pl) == 0)
                    pport = ntohs(pa.sin_port);
                fprintf(stderr,
                        "rdma-trace: SETUP done qpn=%u status=%u peer_port=%u "
                        "fd=%d, back to wait\n",
                        efs_rdma_qpn(conn->rc), conn->rc ? 0u : 1u, pport, conn->fd);
            }
            break;
        }
        case EFS_MSG_RAFT:
            /* The empty reply is transport only, never a consensus ACK. */
            (void)server_raft_host_inbox(payload, payload_len);
            efs_conn_send_msg(conn, EFS_MSG_RAFT_REPLY, NULL, 0);
            break;
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
        case EFS_MSG_RAFT_RESYNC: {
            struct efs_msg_raft_resync_reply r = {.rc = EFS_ERR_INVAL};
            if (payload_len == sizeof(struct efs_msg_raft_resync))
                server_raft_host_resync(payload, &r);
            efs_conn_send_msg(conn, EFS_MSG_RAFT_RESYNC_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_RAFT_STEP_DOWN: {
            struct efs_msg_raft_step_down_reply r = {.rc = EFS_ERR_INVAL, .raft_id = -1};
            if (payload_len == sizeof(struct efs_msg_raft_step_down))
                server_raft_host_step_down(payload, &r);
            efs_conn_send_msg(conn, EFS_MSG_RAFT_STEP_DOWN_REPLY, &r, sizeof(r));
            break;
        }
        /* --- Retired control plane. These opcodes carry no state any
         * more, but they MUST still REPLY: silence costs the sender a full
         * recv timeout (the client GET_FEATURES poll is 2 s x node_count,
         * which wedged every .stats/.find stat for ~16 s). */
        case EFS_MSG_GET_FEATURES: {
            /* Server startup policy applies to this metadata namespace. */
            struct efs_msg_features_reply r;
            memset(&r, 0, sizeof(r));
            uint32_t mask = 0;
            int rc = server_raft_host_query_features_checked(&mask);
            r.features = rc == EFS_OK ? mask : 0;
            /* A transient policy failure must not advertise feature disablement.
             * Clients ignore unavailable discovery and keep their advisory cache;
             * every query still checks fresh authoritative policy. */
            r.status = rc == EFS_OK ? EFS_FEATURES_OK : EFS_FEATURES_UNAVAILABLE;
            efs_conn_send_msg(conn, EFS_MSG_GET_FEATURES_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_QUERY_SNAPSHOT: {
            void *reply=NULL;uint32_t reply_len=0;
            uint32_t version=0;
            if (payload_len>=sizeof(version)) memcpy(&version,payload,sizeof(version));
            int paged=version==EFS_QUERY_PAGE_VERSION && payload_len==sizeof(struct efs_msg_query_snapshot_v4);
            /* Unpaged requests carry only the .stats aggregate (version 1). */
            int stats=version==EFS_QUERY_VERSION && payload_len==sizeof(struct efs_msg_query_snapshot);
            int rc=paged ? server_raft_host_query_page(payload,&reply,&reply_len) : stats ?
                server_raft_host_query_snapshot(payload,&reply,&reply_len) : EFS_ERR_INVAL;
            if (rc) {
                struct efs_msg_query_snapshot_reply failure;
                memset(&failure,0,sizeof(failure));failure.version=EFS_QUERY_VERSION;failure.status=rc;
                if (version==EFS_QUERY_PAGE_VERSION)
                    failure.version=version;
            efs_conn_send_msg(conn,EFS_MSG_QUERY_SNAPSHOT_REPLY,&failure,sizeof(failure));
            } else efs_conn_send_msg(conn,EFS_MSG_QUERY_SNAPSHOT_REPLY,reply,reply_len);
            server_raft_host_query_reply_free(reply);
            break;
        }
        case EFS_MSG_QUERY_STATS: {
            uint8_t status = EFS_QUERY_STATS_UNSUPPORTED;
            efs_conn_send_msg(conn, EFS_MSG_QUERY_STATS_REPLY, &status, 1);
            break;
        }
        default: {
            static uint64_t unknown;
            uint64_t n=__atomic_add_fetch(&unknown,1,__ATOMIC_RELAXED);
            if(n<=10 || !(n%1000))
                fprintf(stderr,"handler: refused unknown message %u; closing (%llu so far)\n",
                        (unsigned)type,(unsigned long long)n);
            close_conn=1;
            break;
        }
        }

        if (rdma_frame)
            efs_rdma_message_release(conn->rc);
        free(to_free);
    }
}
