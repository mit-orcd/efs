#include "client_internal.h"
#include "efs/placement.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* Mark a peer down after this many consecutive failures. */
#define EFS_NODE_DOWN_FAILS 4
/* Skip connect attempts for this long (ms) while marked down.
 * When the timer expires, conn_get clears the streak and re-probes so a
 * restarted peer is picked up again without a client restart.
 * Keep this longer than a typical blackhole connect timeout so small-file
 * meta flushes are not stalled every few seconds by re-probes. */
#define EFS_NODE_DOWN_MS    30000

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

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
        g_client.node_fail_streak[i] = 0;
        g_client.node_down_until_ms[i] = 0;
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

/* Map placement / protocol node id → index in g_client.nodes[]. */
static int node_index_for_id(efs_node_id_t node_id)
{
    if (node_id == 0)
        return -1;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        if (g_client.nodes[i].id == node_id)
            return (int)i;
    }
    return -1;
}

void efs_client_node_note_ok(efs_node_id_t node_id)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0)
        return;
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    g_client.node_fail_streak[idx] = 0;
    g_client.node_down_until_ms[idx] = 0;
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

int efs_client_node_is_down(efs_node_id_t node_id)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0)
        return 1;
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int down = g_client.node_down_until_ms[idx] > monotonic_ms();
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
    return down;
}

void efs_client_nodes_force_reprobe(void)
{
    for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
        pthread_mutex_lock(&g_client.conn_lock[i]);
        g_client.node_fail_streak[i] = 0;
        g_client.node_down_until_ms[i] = 0;
        pthread_mutex_unlock(&g_client.conn_lock[i]);
    }
}

void efs_client_node_note_fail(efs_node_id_t node_id)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0)
        return;
    int64_t now = monotonic_ms();
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    /* Already in cooldown: do not refresh. Active PUTs would otherwise
     * keep pushing down_until forward forever and never re-probe. */
    if (g_client.node_down_until_ms[idx] > now) {
        pthread_mutex_unlock(&g_client.conn_lock[idx]);
        return;
    }
    if (g_client.node_fail_streak[idx] < 1000)
        g_client.node_fail_streak[idx]++;
    if (g_client.node_fail_streak[idx] >= EFS_NODE_DOWN_FAILS)
        g_client.node_down_until_ms[idx] = now + EFS_NODE_DOWN_MS;
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

/* Pool slots can sit in CLOSE-WAIT after the peer FINs (EMFILE, restart,
 * idle timeout). Reusing them looks like a live checkout, then every PUT
 * gets POLLHUP / recv 0 and we report no quorum while the servers are up. */
static int conn_fd_is_dead(int fd)
{
    if (fd < 0)
        return 1;
#ifdef TCP_INFO
    {
        struct tcp_info ti;
        socklen_t il = sizeof(ti);
        if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &il) == 0 &&
            ti.tcpi_state != TCP_ESTABLISHED)
            return 1;
    }
#endif
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int pr = poll(&p, 1, 0);
    if (pr < 0)
        return 1;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
        return 1;
    /* Always peek: IPoIB CLOSE-WAIT sometimes reports no POLLIN/HUP. */
    {
        char b;
        ssize_t n = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0)
            return 1;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return 1;
    }
    int err = 0;
    socklen_t el = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err != 0)
        return 1;
    return 0;
}

int efs_client_conn_get(efs_node_id_t node_id)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0)
        return -1;
    int n = pool_size();

    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int64_t now = monotonic_ms();
    if (g_client.node_down_until_ms[idx] > now) {
        pthread_mutex_unlock(&g_client.conn_lock[idx]);
        return -1;
    }
    /* Cooldown expired — clear streak so a recovered peer gets a clean probe. */
    if (g_client.node_down_until_ms[idx] > 0) {
        g_client.node_down_until_ms[idx] = 0;
        g_client.node_fail_streak[idx] = 0;
    }

    for (;;) {
        int free_slot = -1;
        for (int s = 0; s < n; s++) {
            if (!g_client.conn_busy[idx][s]) {
                free_slot = s;
                break;
            }
        }
        if (free_slot < 0) {
            /* Bounded wait: never hang a FUSE thread forever when the pool is
             * exhausted. After the deadline return a transient net error so the
             * caller backs off/retries instead of deadlocking the mount. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            int wrc = pthread_cond_timedwait(&g_client.conn_cv[idx],
                                             &g_client.conn_lock[idx], &ts);
            if (wrc == ETIMEDOUT) {
                pthread_mutex_unlock(&g_client.conn_lock[idx]);
                return -1;
            }
            continue;
        }

        if (g_client.conn_fd[idx][free_slot] < 0) {
            /* Connect outside the lock: a blackholed peer's connect timeout
             * must not stall every other checkout for this node. Reserve the
             * slot (busy, fd=-1) so waiters block on the cond instead. */
            char host[64];
            uint16_t port = g_client.nodes[idx].port;
            strncpy(host, g_client.nodes[idx].addr, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            g_client.conn_busy[idx][free_slot] = 1;
            pthread_mutex_unlock(&g_client.conn_lock[idx]);

            int fd = efs_connect_tcp(host, port);
            if (fd < 0) {
                pthread_mutex_lock(&g_client.conn_lock[idx]);
                g_client.conn_busy[idx][free_slot] = 0;
                now = monotonic_ms();
                if (g_client.node_fail_streak[idx] < 1000)
                    g_client.node_fail_streak[idx]++;
                if (g_client.node_fail_streak[idx] >= EFS_NODE_DOWN_FAILS &&
                    g_client.node_down_until_ms[idx] <= now)
                    g_client.node_down_until_ms[idx] = now + EFS_NODE_DOWN_MS;
                pthread_cond_signal(&g_client.conn_cv[idx]);
                pthread_mutex_unlock(&g_client.conn_lock[idx]);
                return -1;
            }
            efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
            efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

            pthread_mutex_lock(&g_client.conn_lock[idx]);
            if (g_client.conn_fd[idx][free_slot] >= 0) {
                /* Slot reused while we connected — keep the existing fd. */
                close(fd);
                fd = g_client.conn_fd[idx][free_slot];
            } else {
                g_client.conn_fd[idx][free_slot] = fd;
            }
            g_client.node_fail_streak[idx] = 0;
            g_client.node_down_until_ms[idx] = 0;
            /* busy already set */
            pthread_mutex_unlock(&g_client.conn_lock[idx]);
            return fd;
        }

        int fd = g_client.conn_fd[idx][free_slot];
        if (conn_fd_is_dead(fd)) {
            close(fd);
            g_client.conn_fd[idx][free_slot] = -1;
            continue;
        }
        g_client.conn_busy[idx][free_slot] = 1;
        pthread_mutex_unlock(&g_client.conn_lock[idx]);
        return fd;
    }
}

void efs_client_conn_release(efs_node_id_t node_id, int fd)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0 || fd < 0)
        return;
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int s = slot_for_fd((uint32_t)idx, fd);
    if (s >= 0)
        g_client.conn_busy[idx][s] = 0;
    pthread_cond_signal(&g_client.conn_cv[idx]);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

void efs_client_conn_invalidate_node(efs_node_id_t node_id)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0)
        return;
    int n = pool_size();
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    for (int s = 0; s < n; s++) {
        if (g_client.conn_busy[idx][s])
            continue;
        if (g_client.conn_fd[idx][s] >= 0) {
            close(g_client.conn_fd[idx][s]);
            g_client.conn_fd[idx][s] = -1;
        }
    }
    pthread_cond_broadcast(&g_client.conn_cv[idx]);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

void efs_client_conn_drop(efs_node_id_t node_id, int fd)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0 || fd < 0)
        return;
    int n = pool_size();
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int s = slot_for_fd((uint32_t)idx, fd);
    if (s >= 0) {
        if (g_client.conn_fd[idx][s] >= 0)
            close(g_client.conn_fd[idx][s]);
        g_client.conn_fd[idx][s] = -1;
        g_client.conn_busy[idx][s] = 0;
    } else {
        close(fd);
    }
    /* After efsd restart the whole idle pool for this peer is usually dead;
     * flush it so the next checkout opens fresh TCP connections. */
    for (int i = 0; i < n; i++) {
        if (g_client.conn_busy[idx][i])
            continue;
        if (g_client.conn_fd[idx][i] >= 0) {
            close(g_client.conn_fd[idx][i]);
            g_client.conn_fd[idx][i] = -1;
        }
    }
    pthread_cond_broadcast(&g_client.conn_cv[idx]);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

void efs_client_shutdown(void)
{
    static int done;
    if (done)
        return;
    done = 1;

    /* Persist any write-combined chunks before tearing down connections. */
    (void)efs_dcache_flush_all();

    /* Stop the flush thread before freeing state it might be using; join
     * blocks until any in-flight flush completes. */
    efs_client_stop_meta_flush();

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
    free(g_client.meta_slot_hashes[0]);
    free(g_client.meta_slot_hashes[1]);
    free(g_client.meta_slot_sums[0]);
    free(g_client.meta_slot_sums[1]);
    g_client.meta_slot_hashes[0] = NULL;
    g_client.meta_slot_hashes[1] = NULL;
    g_client.meta_slot_sums[0] = NULL;
    g_client.meta_slot_sums[1] = NULL;
    g_client.meta_slot_pages[0] = 0;
    g_client.meta_slot_pages[1] = 0;
    g_client.meta_slot_ino_pages[0] = 0;
    g_client.meta_slot_ino_pages[1] = 0;
    g_client.meta_slot_chunk_pages[0] = 0;
    g_client.meta_slot_chunk_pages[1] = 0;

    efs_export_free(&g_client.export);
    free(g_client.meta_cache_blob);
    free(g_client.meta_cache_ch);
    g_client.meta_cache_blob = NULL;
    g_client.meta_cache_ch = NULL;
    g_client.meta_cache_cap = 0;
    g_client.meta_cache_ch_cap = 0;
    g_client.meta_cache_len = 0;
    pthread_mutex_destroy(&g_client.lock);
    if (g_client.dir_locks_ready) {
        for (int i = 0; i < EFS_DIR_LOCKS; i++)
            pthread_mutex_destroy(&g_client.dir_lock[i]);
        pthread_mutex_destroy(&g_client.dirty_mu);
        pthread_mutex_destroy(&g_client.idx_mu);
        g_client.dir_locks_ready = 0;
    }
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
