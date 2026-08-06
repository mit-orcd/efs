#include "client_internal.h"
#include "efs/placement.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>

static int parse_host_port(const char *str, char *host, size_t host_len, uint16_t *port)
{
    const char *colon = strrchr(str, ':');
    if (!colon)
        return -1;
    size_t hlen = (size_t)(colon - str);
    if (hlen >= host_len)
        return -1;
    memcpy(host, str, hlen);
    host[hlen] = '\0';
    *port = (uint16_t)atoi(colon + 1);
    return 0;
}

static bool resolve_local(const char *host)
{
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        hostname[0] = '\0';
    hostname[sizeof(hostname) - 1] = '\0';

    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 ||
        strcmp(host, "::1") == 0 || strcmp(host, hostname) == 0)
        return true;

    struct hostent *he = gethostbyname(host);
    if (he) {
        struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
        for (int i = 0; addr_list[i] != NULL; i++) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, addr_list[i], ip, sizeof(ip));
            if (strcmp(ip, "127.0.0.1") == 0)
                return true;
        }
    }
    return false;
}

static int conn_pool_inited;

static int pool_size(void)
{
    int n = g_client.conn_pool_size;
    if (n < 1)
        n = EFS_CLIENT_CONNS_PER_NODE;
    if (n > EFS_CLIENT_CONNS_PER_NODE)
        n = EFS_CLIENT_CONNS_PER_NODE;
    return n;
}

void efs_client_conn_init(void)
{
    const char *env = getenv("EFS_CLIENT_CONNS_PER_NODE");
    int n = EFS_CLIENT_CONNS_PER_NODE;
    if (env && *env) {
        int v = atoi(env);
        if (v >= 1 && v <= EFS_CLIENT_CONNS_PER_NODE)
            n = v;
    }
    g_client.conn_pool_size = n;

    for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
        for (int s = 0; s < EFS_CLIENT_CONNS_PER_NODE; s++) {
            /* First init: BSS zeros look like fd 0 — do not close. */
            if (conn_pool_inited && g_client.conn_fd[i][s] >= 0)
                close(g_client.conn_fd[i][s]);
            g_client.conn_fd[i][s] = -1;
            g_client.conn_busy[i][s] = 0;
        }
        if (!conn_pool_inited) {
            pthread_mutex_init(&g_client.conn_lock[i], NULL);
            pthread_cond_init(&g_client.conn_cv[i], NULL);
        }
    }
    conn_pool_inited = 1;
}

static int slot_for_fd(uint32_t idx, int fd)
{
    int n = pool_size();
    for (int s = 0; s < n; s++) {
        if (g_client.conn_fd[idx][s] == fd)
            return s;
    }
    return -1;
}

int efs_client_conn_get(efs_node_id_t node_id)
{
    if (node_id == 0 || node_id > g_client.node_count)
        return -1;
    uint32_t idx = node_id - 1;
    int n = pool_size();

    pthread_mutex_lock(&g_client.conn_lock[idx]);
    for (;;) {
        int free_slot = -1;
        for (int s = 0; s < n; s++) {
            if (!g_client.conn_busy[idx][s]) {
                free_slot = s;
                break;
            }
        }
        if (free_slot < 0) {
            pthread_cond_wait(&g_client.conn_cv[idx], &g_client.conn_lock[idx]);
            continue;
        }

        if (g_client.conn_fd[idx][free_slot] < 0) {
            struct efs_node *node = &g_client.nodes[idx];
            int fd = efs_connect_tcp(node->addr, node->port);
            if (fd < 0) {
                pthread_mutex_unlock(&g_client.conn_lock[idx]);
                return -1;
            }
            efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
            efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
            g_client.conn_fd[idx][free_slot] = fd;
        }

        g_client.conn_busy[idx][free_slot] = 1;
        int fd = g_client.conn_fd[idx][free_slot];
        pthread_mutex_unlock(&g_client.conn_lock[idx]);
        return fd;
    }
}

void efs_client_conn_release(efs_node_id_t node_id, int fd)
{
    if (node_id == 0 || node_id > g_client.node_count || fd < 0)
        return;
    uint32_t idx = node_id - 1;
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int s = slot_for_fd(idx, fd);
    if (s >= 0)
        g_client.conn_busy[idx][s] = 0;
    pthread_cond_signal(&g_client.conn_cv[idx]);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

void efs_client_conn_drop(efs_node_id_t node_id, int fd)
{
    if (node_id == 0 || node_id > g_client.node_count || fd < 0)
        return;
    uint32_t idx = node_id - 1;
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int s = slot_for_fd(idx, fd);
    if (s >= 0) {
        if (g_client.conn_fd[idx][s] >= 0)
            close(g_client.conn_fd[idx][s]);
        g_client.conn_fd[idx][s] = -1;
        g_client.conn_busy[idx][s] = 0;
    } else {
        close(fd);
    }
    pthread_cond_signal(&g_client.conn_cv[idx]);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

void efs_client_shutdown(void)
{
    static int done;
    if (done)
        return;
    done = 1;

    if (conn_pool_inited) {
        for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
            pthread_mutex_lock(&g_client.conn_lock[i]);
            for (int s = 0; s < EFS_CLIENT_CONNS_PER_NODE; s++) {
                if (g_client.conn_fd[i][s] >= 0) {
                    close(g_client.conn_fd[i][s]);
                    g_client.conn_fd[i][s] = -1;
                }
                g_client.conn_busy[i][s] = 0;
            }
            pthread_mutex_unlock(&g_client.conn_lock[i]);
            pthread_cond_destroy(&g_client.conn_cv[i]);
            pthread_mutex_destroy(&g_client.conn_lock[i]);
        }
        conn_pool_inited = 0;
    }

    free(g_client.dirty_ino_keys);
    free(g_client.dirty_chunk_keys);
    free(g_client.dirty_chunk_inos);
    free(g_client.dirty_chunk_idxs);
    g_client.dirty_ino_keys = NULL;
    g_client.dirty_chunk_keys = NULL;
    g_client.dirty_chunk_inos = NULL;
    g_client.dirty_chunk_idxs = NULL;
    g_client.dirty_ino_mask = 0;
    g_client.dirty_chunk_mask = 0;
    g_client.dirty_ino_count = 0;
    g_client.dirty_chunk_count = 0;
    g_client.dirty_chunk_cap = 0;

    efs_export_free(&g_client.export);
    pthread_mutex_destroy(&g_client.lock);
}

void efs_client_init_nodes(struct efs_client *c, const char *node_list[EFS_MAX_NODES],
                           uint32_t node_count)
{
    c->node_count = node_count;
    c->local_node_id = 0;

    for (uint32_t i = 0; i < node_count; i++) {
        char host[64];
        uint16_t port = 0;
        if (parse_host_port(node_list[i], host, sizeof(host), &port) != 0) {
            fprintf(stderr, "Invalid node address: %s\n", node_list[i]);
            continue;
        }

        c->nodes[i].id = i + 1;
        strncpy(c->nodes[i].addr, host, sizeof(c->nodes[i].addr) - 1);
        c->nodes[i].addr[sizeof(c->nodes[i].addr) - 1] = '\0';
        c->nodes[i].port = port;

        if (resolve_local(host)) {
            c->local_node_id = i + 1;
        }
    }
    efs_client_conn_init();
}

int efs_client_discover_nodes(struct efs_client *c, const char *host, uint16_t port)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_send_msg(fd, EFS_MSG_LIST_NODES, NULL, 0) != 0 ||
        efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
        type != EFS_MSG_LIST_NODES_REPLY ||
        payload_len != sizeof(struct efs_msg_list_nodes_reply)) {
        free(payload);
        close(fd);
        return -1;
    }

    struct efs_msg_list_nodes_reply *reply = payload;
    uint32_t count = reply->node_count;
    if (count == 0 || count > EFS_MAX_NODES) {
        free(payload);
        close(fd);
        return -1;
    }

    c->node_count = count;
    c->local_node_id = 0;
    for (uint32_t i = 0; i < count; i++) {
        c->nodes[i] = reply->nodes[i];
        c->nodes[i].addr[sizeof(c->nodes[i].addr) - 1] = '\0';
        if (resolve_local(c->nodes[i].addr))
            c->local_node_id = c->nodes[i].id;
    }

    free(payload);
    close(fd);
    efs_client_conn_init();
    return 0;
}
