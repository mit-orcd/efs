#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int server_join_cluster(struct efsd_server *s, const char *peer_host, uint16_t peer_port)
{
    struct efs_conn *pc = server_peer_conn_get(peer_host, peer_port);
    if (!pc) {
        fprintf(stderr, "Cannot connect to peer %s:%u\n", peer_host, peer_port);
        return -1;
    }

    struct efs_msg_hello h;
    memset(&h, 0, sizeof(h));
    h.version = EFS_VERSION_PACK;
    strncpy(h.build_id, EFS_BUILD_ID, sizeof(h.build_id) - 1);
    h.node_id = s->id;
    strncpy(h.addr, s->addr, sizeof(h.addr) - 1);
    h.port = s->port;
    server_format_storage_paths(s, h.storage_path, sizeof(h.storage_path));
    h.quota = s->quota;
    {
        struct efs_node *local = server_local_node(s);
        h.used = local ? local->used : server_compute_local_usage(s);
    }

    if (efs_conn_send_msg(pc, EFS_MSG_HELLO, &h, sizeof(h)) != 0) {
        fprintf(stderr, "Failed to send HELLO to peer %s:%u\n", peer_host, peer_port);
        server_peer_conn_drop(peer_host, peer_port, pc);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_conn_recv_msg(pc, &type, &payload, &payload_len) != 0) {
        fprintf(stderr, "No HELLO_ACK from peer %s:%u (timeout or disconnect)\n",
                peer_host, peer_port);
        server_peer_conn_drop(peer_host, peer_port, pc);
        return -1;
    }
    if (type != EFS_MSG_HELLO_ACK) {
        fprintf(stderr, "Peer %s:%u replied with unexpected message type %u\n",
                peer_host, peer_port, type);
        free(payload);
        server_peer_conn_drop(peer_host, peer_port, pc);
        return -1;
    }
    if (payload_len != sizeof(struct efs_msg_hello_ack)) {
        fprintf(stderr, "Peer %s:%u sent malformed HELLO_ACK (len %u)\n",
                peer_host, peer_port, payload_len);
        free(payload);
        server_peer_conn_drop(peer_host, peer_port, pc);
        return -1;
    }

    struct efs_msg_hello_ack *ack = payload;
    if (ack->assigned_id == 0 || ack->assigned_id != s->id) {
        if (ack->reject_reason == EFS_HELLO_REJECT_VERSION) {
            fprintf(stderr,
                    "Join refused by %s:%u — VERSION MISMATCH: this node runs "
                    "build '%s', the cluster runs build '%s'. Run the same "
                    "efsd build on every node.\n",
                    peer_host, peer_port, EFS_BUILD_ID, ack->build_id);
        } else {
            fprintf(stderr,
                    "Join rejected by peer %s:%u (cluster full or id mismatch; "
                    "assigned_id=%llu local_id=%llu count=%u)\n",
                    peer_host, peer_port,
                    (unsigned long long)ack->assigned_id,
                    (unsigned long long)s->id, ack->node_count);
        }
        free(payload);
        server_peer_conn_drop(peer_host, peer_port, pc);
        return -1;
    }

    int self_in_list = 0;
    for (uint32_t i = 0; i < ack->node_count && i < EFS_MAX_NODES; i++) {
        if (ack->nodes[i].id == s->id) {
            self_in_list = 1;
            break;
        }
    }
    if (!self_in_list) {
        fprintf(stderr,
                "Join failed: peer %s:%u ACK omitted this node (id %llu)\n",
                peer_host, peer_port, (unsigned long long)s->id);
        free(payload);
        server_peer_conn_drop(peer_host, peer_port, pc);
        return -1;
    }

    uint32_t joined = ack->node_count;
    pthread_mutex_lock(&s->lock);
    /* Keep our cached used across membership adopt (peer view can be stale). */
    uint64_t keep_used = 0;
    {
        struct efs_node *prev = server_local_node(s);
        if (prev)
            keep_used = prev->used;
    }
    s->epoch = ack->epoch;
    s->node_count = ack->node_count;
    if (s->node_count > EFS_MAX_NODES)
        s->node_count = EFS_MAX_NODES;
    memcpy(s->nodes, ack->nodes, sizeof(s->nodes));
    server_dedupe_nodes_locked(s);
    /* Keep this process's listen/storage facts authoritative after adopt. */
    struct efs_node *local = server_local_node(s);
    if (!local && s->node_count < EFS_MAX_NODES) {
        local = &s->nodes[s->node_count++];
        memset(local, 0, sizeof(*local));
        local->id = s->id;
    }
    if (local) {
        strncpy(local->addr, s->addr, sizeof(local->addr) - 1);
        local->addr[sizeof(local->addr) - 1] = '\0';
        local->port = s->port;
        server_format_storage_paths(s, local->storage_path, sizeof(local->storage_path));
        local->quota = s->quota;
        local->used = keep_used;
    }
    server_dedupe_nodes_locked(s);
    joined = s->node_count;
    server_nodes_mark_dirty(s);
    pthread_mutex_unlock(&s->lock);
    server_nodes_flush_dirty(s);
    free(payload);
    server_peer_conn_release(peer_host, peer_port, pc);

    printf("Joined cluster with %u nodes\n", joined);
    return 0;
}

/* Forward one membership update to a single peer (best-effort, pooled conn). */
static void gossip_hello_to(struct efsd_server *s, const struct efs_msg_hello *h,
                            const char *host, uint16_t port)
{
    (void)s;
    struct in_addr a;
    if (!host || inet_pton(AF_INET, host, &a) != 1)
        return;
    struct efs_conn *pc = server_peer_conn_get(host, port);
    if (!pc)
        return;
    if (efs_conn_send_msg(pc, EFS_MSG_HELLO, h, sizeof(*h)) == 0) {
        uint8_t type;
        void *payload = NULL;
        uint32_t payload_len = 0;
        int rc = efs_conn_recv_msg(pc, &type, &payload, &payload_len);
        free(payload);
        if (rc == 0) {
            server_peer_conn_release(host, port, pc);
            return;
        }
    }
    server_peer_conn_drop(host, port, pc);
}

/* Broadcast a membership change (join/refresh) to every other known peer so
 * all nodes converge on the same placement ring, not just the contacted one.
 * Call WITHOUT holding s->lock. */
void server_gossip_membership(struct efsd_server *s, const struct efs_msg_hello *h)
{
    struct efs_node nodes[EFS_MAX_NODES];
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;
    memcpy(nodes, s->nodes, sizeof(struct efs_node) * node_count);
    efs_node_id_t self = s->id;
    pthread_mutex_unlock(&s->lock);

    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == self || nodes[i].id == h->node_id)
            continue;
        gossip_hello_to(s, h, nodes[i].addr, nodes[i].port);
    }
}

/* Liveness probes use private per-peer connections (server_peer_conn_new),
 * not the bounded pool: when forwarded client commands filled the pool the
 * heartbeat queued behind them and marked healthy peers down ("unresponsive
 * — marked down for 30s" on every node, Sep 21). Only the heartbeat thread
 * touches hb_conn[]. */
static struct efs_conn *hb_conn[EFS_MAX_NODES];
static char hb_host[EFS_MAX_NODES][64];
static uint16_t hb_port[EFS_MAX_NODES];

static void hb_conn_drop(uint32_t slot)
{
    if (slot < EFS_MAX_NODES && hb_conn[slot]) {
        efs_conn_destroy(hb_conn[slot]);
        hb_conn[slot] = NULL;
    }
}

/* Slot = index in the node table; indices may shift on membership
 * change, so a cached conn is reused only for the same host:port. */
static struct efs_conn *hb_conn_for(uint32_t slot, const char *host,
                                    uint16_t port)
{
    if (slot >= EFS_MAX_NODES)
        return NULL;
    if (hb_conn[slot] &&
        (hb_port[slot] != port || strcmp(hb_host[slot], host) != 0))
        hb_conn_drop(slot);
    if (!hb_conn[slot]) {
        hb_conn[slot] = server_peer_conn_new(host, port);
        if (hb_conn[slot]) {
            snprintf(hb_host[slot], sizeof(hb_host[slot]), "%s", host);
            hb_port[slot] = port;
        }
    }
    return hb_conn[slot];
}

static int send_heartbeat(uint32_t slot, const char *host, uint16_t port)
{
    /* Skip non-numeric peers: avoids NSS. IB clusters advertise IPv4. */
    struct in_addr a;
    if (!host || inet_pton(AF_INET, host, &a) != 1)
        return -1;

    struct efs_conn *pc = hb_conn_for(slot, host, port);
    if (!pc)
        return -1;

    if (efs_conn_send_msg(pc, EFS_MSG_HEARTBEAT, NULL, 0) != 0) {
        hb_conn_drop(slot);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    int rc = efs_conn_recv_msg(pc, &type, &payload, &payload_len);
    free(payload);
    if (rc != 0) {
        hb_conn_drop(slot);
        return -1;
    }
    return (type == EFS_MSG_HEARTBEAT_ACK) ? 0 : -1;
}

static uint64_t hb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Heartbeat thread: ping peers on the pooled conn, track fail streaks, and
 * mark unresponsive nodes down (excluded from placement) with a cooldown so a
 * dead node does not keep absorbing PUT/placement forever. No usage flush here. */
#define EFS_HB_DOWN_FAILS 3
#define EFS_HB_DOWN_MS    30000
static void *heartbeat_thread(void *arg)
{
    struct efsd_server *s = arg;
    /* Heap: efs_node is ~4 KiB (storage_path); EFS_MAX_NODES of them on the
     * stack plus usage_save path buffers blew the ASAN-instrumented stack. */
    struct efs_node *nodes = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
    if (!nodes)
        return NULL;

    uint64_t gc_log_ms = 0;
    while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
        usleep(EFS_HEARTBEAT_MS * 1000);

        pthread_mutex_lock(&s->lock);
        uint32_t node_count = s->node_count;
        if (node_count > EFS_MAX_NODES)
            node_count = EFS_MAX_NODES;
        memcpy(nodes, s->nodes, sizeof(struct efs_node) * node_count);
        efs_node_id_t self = s->id;
        pthread_mutex_unlock(&s->lock);

        uint64_t now = hb_now_ms();
        if (now - gc_log_ms >= 15000) {
            struct efs_msg_gc_status_reply gc;
            server_gc_status(s, &gc);
            fprintf(stderr, "gc-health: stage=%u group=%u elapsed_ms=%llu last_pass_age_ms=%llu "
                    "passes=%llu sampled=%u pending=%llu,%llu reap=%llu,%llu removed=%llu "
                    "file_bytes=%llu payload_bytes=%llu delete_errors=%llu sweep_errors=%llu "
                    "reap_errors=%llu ack_errors=%llu missing_exports=%llu\n",
                    gc.stage, gc.group, (unsigned long long)gc.pass_elapsed_ms,
                    (unsigned long long)gc.last_pass_age_ms, (unsigned long long)gc.passes,
                    gc.sampled_mask, (unsigned long long)gc.pending[0], (unsigned long long)gc.pending[1],
                    (unsigned long long)gc.reap_seen[0], (unsigned long long)gc.reap_seen[1],
                    (unsigned long long)gc.removed_fragments, (unsigned long long)gc.removed_file_bytes,
                    (unsigned long long)gc.removed_payload_bytes, (unsigned long long)gc.delete_errors,
                    (unsigned long long)gc.sweep_errors, (unsigned long long)gc.reap_errors,
                    (unsigned long long)gc.ack_errors, (unsigned long long)gc.missing_exports);
            gc_log_ms = now;
        }
        for (uint32_t i = 0; i < node_count; i++) {
            if (nodes[i].id == self)
                continue;
            int ok = (send_heartbeat(i, nodes[i].addr, nodes[i].port) == 0);

            /* Record outcome under the lock by node id (indices may shift). */
            pthread_mutex_lock(&s->lock);
            for (uint32_t j = 0; j < s->node_count; j++) {
                if (s->nodes[j].id != nodes[i].id)
                    continue;
                if (ok) {
                    s->nodes[j].hb_fail_streak = 0;
                    s->nodes[j].down_until_ms = 0;
                } else {
                    if (s->nodes[j].hb_fail_streak < 1000)
                        s->nodes[j].hb_fail_streak++;
                    if (s->nodes[j].hb_fail_streak >= EFS_HB_DOWN_FAILS &&
                        s->nodes[j].down_until_ms <= now) {
                        s->nodes[j].down_until_ms = now + EFS_HB_DOWN_MS;
                        fprintf(stderr,
                                "heartbeat: node %u (%s:%u) unresponsive — "
                                "marked down for %ds\n",
                                s->nodes[j].id, s->nodes[j].addr,
                                s->nodes[j].port, EFS_HB_DOWN_MS / 1000);
                    }
                }
                break;
            }
            pthread_mutex_unlock(&s->lock);
        }
    }
    free(nodes);
    return NULL;
}

void server_start_heartbeat(struct efsd_server *s)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (efsd_pthread_create(&tid, heartbeat_thread, s) == 0) {
        fprintf(stderr, "heartbeat: enabled (%d ms interval)\n", EFS_HEARTBEAT_MS);
    }
    pthread_attr_destroy(&attr);
}

/* Rejoin thread: periodically retry joining until this server is part of a
 * cluster with at least one other node. Uses the explicit --join target when
 * set, otherwise the persisted peer list. */
static void *rejoin_thread(void *arg)
{
    struct efsd_server *s = arg;
    while (s->running) {
        sleep(2);

        pthread_mutex_lock(&s->lock);
        uint32_t node_count = s->node_count;
        pthread_mutex_unlock(&s->lock);

        /* Already in a cluster with at least one peer: nothing to do. */
        if (node_count > 1)
            break;

        int rc;
        if (s->rejoin_addr[0]) {
            rc = server_join_cluster(s, s->rejoin_addr, s->rejoin_port);
            if (rc == 0) {
                printf("Rejoined cluster via %s:%u\n", s->rejoin_addr, s->rejoin_port);
                pthread_mutex_lock(&s->lock);
                server_nodes_mark_dirty(s);
                pthread_mutex_unlock(&s->lock);
                server_nodes_flush_dirty(s);
            }
        } else {
            rc = server_rejoin_cluster(s);
            if (rc == 0) {
                pthread_mutex_lock(&s->lock);
                server_nodes_mark_dirty(s);
                pthread_mutex_unlock(&s->lock);
                server_nodes_flush_dirty(s);
            }
        }
    }
    return NULL;
}

void server_start_rejoin(struct efsd_server *s)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&s->rejoin_tid, &attr, rejoin_thread, s);
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
    server_nodes_mark_dirty(s);
    pthread_mutex_unlock(&s->lock);
    server_nodes_flush_dirty(s);
}
