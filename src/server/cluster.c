#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int server_join_cluster(struct efsd_server *s, const char *peer_host, uint16_t peer_port)
{
    int fd = efs_connect_tcp(peer_host, peer_port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to peer %s:%u\n", peer_host, peer_port);
        return -1;
    }

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_hello h;
    memset(&h, 0, sizeof(h));
    h.version = (EFS_VERSION_MAJOR << 16) | (EFS_VERSION_MINOR << 8) | EFS_VERSION_PATCH;
    h.node_id = s->id;
    strncpy(h.addr, s->addr, sizeof(h.addr) - 1);
    h.port = s->port;
    strncpy(h.storage_path, s->storage_path, sizeof(h.storage_path) - 1);
    h.quota = s->quota;
    {
        struct efs_node *local = server_local_node(s);
        h.used = local ? local->used : server_compute_local_usage(s);
    }

    if (efs_send_msg(fd, EFS_MSG_HELLO, &h, sizeof(h)) != 0) {
        fprintf(stderr, "Failed to send HELLO to peer %s:%u\n", peer_host, peer_port);
        close(fd);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0) {
        fprintf(stderr, "No HELLO_ACK from peer %s:%u (timeout or disconnect)\n",
                peer_host, peer_port);
        close(fd);
        return -1;
    }
    if (type != EFS_MSG_HELLO_ACK) {
        fprintf(stderr, "Peer %s:%u replied with unexpected message type %u\n",
                peer_host, peer_port, type);
        free(payload);
        close(fd);
        return -1;
    }
    if (payload_len != sizeof(struct efs_msg_hello_ack)) {
        fprintf(stderr, "Peer %s:%u sent malformed HELLO_ACK (len %u)\n",
                peer_host, peer_port, payload_len);
        free(payload);
        close(fd);
        return -1;
    }

    struct efs_msg_hello_ack *ack = payload;
    if (ack->assigned_id == 0 || ack->assigned_id != s->id) {
        fprintf(stderr,
                "Join rejected by peer %s:%u (cluster full or id mismatch; "
                "assigned_id=%llu local_id=%llu count=%u)\n",
                peer_host, peer_port,
                (unsigned long long)ack->assigned_id,
                (unsigned long long)s->id, ack->node_count);
        free(payload);
        close(fd);
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
        close(fd);
        return -1;
    }

    uint32_t joined = ack->node_count;
    pthread_mutex_lock(&s->lock);
    s->epoch = ack->epoch;
    s->node_count = ack->node_count;
    memcpy(s->nodes, ack->nodes, sizeof(s->nodes));
    /* Keep this process's listen/storage facts authoritative after adopt. */
    struct efs_node *local = server_local_node(s);
    if (local) {
        strncpy(local->addr, s->addr, sizeof(local->addr) - 1);
        local->addr[sizeof(local->addr) - 1] = '\0';
        local->port = s->port;
        strncpy(local->storage_path, s->storage_path, sizeof(local->storage_path) - 1);
        local->storage_path[sizeof(local->storage_path) - 1] = '\0';
        local->quota = s->quota;
        local->used = server_compute_local_usage(s);
    }
    server_save_nodes(s);
    pthread_mutex_unlock(&s->lock);
    free(payload);
    close(fd);

    printf("Joined cluster with %u nodes\n", joined);
    return 0;
}

static int send_heartbeat(const char *host, uint16_t port)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    if (efs_send_msg(fd, EFS_MSG_HEARTBEAT, NULL, 0) != 0) {
        close(fd);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    int rc = efs_recv_msg(fd, &type, &payload, &payload_len);
    free(payload);
    close(fd);
    return (rc == 0 && type == EFS_MSG_HEARTBEAT_ACK) ? 0 : -1;
}

/* Heartbeat thread: periodically ping all peers to keep the cluster alive. */
static void *heartbeat_thread(void *arg)
{
    struct efsd_server *s = arg;
    while (s->running) {
        usleep(EFS_HEARTBEAT_MS * 1000);

        pthread_mutex_lock(&s->lock);
        struct efs_node nodes[EFS_MAX_NODES];
        uint32_t node_count = s->node_count;
        memcpy(nodes, s->nodes, sizeof(nodes));
        pthread_mutex_unlock(&s->lock);

        for (uint32_t i = 0; i < node_count; i++) {
            if (nodes[i].id == s->id)
                continue;
            send_heartbeat(nodes[i].addr, nodes[i].port);
        }
    }
    return NULL;
}

void server_start_heartbeat(struct efsd_server *s)
{
    pthread_t tid;
    pthread_create(&tid, NULL, heartbeat_thread, s);
    pthread_detach(tid);
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
                server_fetch_metadata_from(s, s->rejoin_addr, s->rejoin_port);
                pthread_mutex_lock(&s->lock);
                server_save_nodes(s);
                pthread_mutex_unlock(&s->lock);
            }
        } else {
            rc = server_rejoin_cluster(s);
            if (rc == 0) {
                pthread_mutex_lock(&s->lock);
                server_save_nodes(s);
                pthread_mutex_unlock(&s->lock);
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
