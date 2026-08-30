#include "server_internal.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* Persistent server→server pool. Same idea as the client conn pool:
 * connect once per peer, upgrade to RDMA when the HCA is usable, reuse
 * across meta rebuild/flush/migrate. Drop on protocol/net errors so the
 * next checkout reconnects. */

/* Callers BLOCK on the condvar below once every connection to a peer is busy,
 * so this is the hard ceiling on server->server concurrency. At 4 it capped the
 * parallel metadata flush at 4-way no matter how many shard tables were fanned
 * out, which is most of why flushing 62 tables still took ~65 ms. */
#define EFS_PEER_CONNS_PER_NODE 32
/* After this many consecutive connect failures, stop reconnecting for the
 * cooldown window so a dead peer does not stall every caller on connect(). */
#define EFS_PEER_DOWN_FAILS 3
#define EFS_PEER_DOWN_MS    10000

struct peer_slot {
    char host[64];
    uint16_t port;
    int in_use; /* host/port assigned */
    struct efs_conn *conn[EFS_PEER_CONNS_PER_NODE];
    int busy[EFS_PEER_CONNS_PER_NODE];
    uint64_t down_until_ms; /* skip reconnect until this monotonic deadline */
    uint32_t fail_streak;
    pthread_mutex_t lock;
    pthread_cond_t cv;
};

static uint64_t peer_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static struct peer_slot g_peers[EFS_MAX_NODES];
static pthread_mutex_t g_peers_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_peers_inited;

void server_peer_pool_init(void)
{
    pthread_mutex_lock(&g_peers_mu);
    if (g_peers_inited) {
        pthread_mutex_unlock(&g_peers_mu);
        return;
    }
    for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
        memset(&g_peers[i], 0, sizeof(g_peers[i]));
        pthread_mutex_init(&g_peers[i].lock, NULL);
        pthread_cond_init(&g_peers[i].cv, NULL);
    }
    g_peers_inited = 1;
    pthread_mutex_unlock(&g_peers_mu);
}

void server_peer_pool_shutdown(void)
{
    pthread_mutex_lock(&g_peers_mu);
    if (!g_peers_inited) {
        pthread_mutex_unlock(&g_peers_mu);
        return;
    }
    for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
        struct peer_slot *p = &g_peers[i];
        pthread_mutex_lock(&p->lock);
        for (int s = 0; s < EFS_PEER_CONNS_PER_NODE; s++) {
            if (p->conn[s]) {
                efs_conn_destroy(p->conn[s]);
                p->conn[s] = NULL;
            }
            p->busy[s] = 0;
        }
        p->in_use = 0;
        p->host[0] = '\0';
        p->port = 0;
        pthread_mutex_unlock(&p->lock);
    }
    g_peers_inited = 0;
    pthread_mutex_unlock(&g_peers_mu);
}

static struct peer_slot *peer_slot_for(const char *host, uint16_t port)
{
    if (!host || !*host || port == 0)
        return NULL;
    if (!g_peers_inited)
        server_peer_pool_init();

    pthread_mutex_lock(&g_peers_mu);
    struct peer_slot *free_slot = NULL;
    for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
        struct peer_slot *p = &g_peers[i];
        if (p->in_use && p->port == port && strcmp(p->host, host) == 0) {
            pthread_mutex_unlock(&g_peers_mu);
            return p;
        }
        if (!p->in_use && !free_slot)
            free_slot = p;
    }
    if (!free_slot) {
        pthread_mutex_unlock(&g_peers_mu);
        return NULL;
    }
    strncpy(free_slot->host, host, sizeof(free_slot->host) - 1);
    free_slot->host[sizeof(free_slot->host) - 1] = '\0';
    free_slot->port = port;
    free_slot->in_use = 1;
    pthread_mutex_unlock(&g_peers_mu);
    return free_slot;
}

static struct efs_conn *peer_connect(const char *host, uint16_t port)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return NULL;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    struct efs_conn *nc = efs_conn_wrap_tcp(fd, 0);
    if (!nc) {
        close(fd);
        return NULL;
    }
    if (efs_rdma_available() &&
        efs_rdma_client_upgrade(nc) != 0 &&
        efs_rdma_transport() == EFS_TRANSPORT_RDMA) {
        efs_conn_destroy(nc);
        return NULL;
    }
    return nc;
}

struct efs_conn *server_peer_conn_get(const char *host, uint16_t port)
{
    struct peer_slot *p = peer_slot_for(host, port);
    if (!p)
        return NULL;

    uint64_t now = peer_now_ms();
    pthread_mutex_lock(&p->lock);
    /* Down-cooldown: don't even try to connect to a peer that has failed
     * repeatedly; the caller treats NULL as "peer down" and skips it fast. */
    if (p->down_until_ms > now) {
        pthread_mutex_unlock(&p->lock);
        return NULL;
    }
    for (;;) {
        int free_s = -1;
        for (int s = 0; s < EFS_PEER_CONNS_PER_NODE; s++) {
            if (!p->busy[s]) {
                free_s = s;
                break;
            }
        }
        if (free_s < 0) {
            /* Bounded wait: never park a caller forever on a busy pool; after
             * the deadline let the caller fail/back off. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            int wrc = pthread_cond_timedwait(&p->cv, &p->lock, &ts);
            if (wrc == ETIMEDOUT) {
                pthread_mutex_unlock(&p->lock);
                return NULL;
            }
            continue;
        }

        if (!p->conn[free_s]) {
            char h[64];
            uint16_t pt = p->port;
            strncpy(h, p->host, sizeof(h) - 1);
            h[sizeof(h) - 1] = '\0';
            p->busy[free_s] = 1;
            pthread_mutex_unlock(&p->lock);

            struct efs_conn *nc = peer_connect(h, pt);
            if (!nc) {
                pthread_mutex_lock(&p->lock);
                p->busy[free_s] = 0;
                if (p->fail_streak < 1000)
                    p->fail_streak++;
                if (p->fail_streak >= EFS_PEER_DOWN_FAILS)
                    p->down_until_ms = peer_now_ms() + EFS_PEER_DOWN_MS;
                pthread_cond_signal(&p->cv);
                pthread_mutex_unlock(&p->lock);
                return NULL;
            }

            pthread_mutex_lock(&p->lock);
            if (p->conn[free_s]) {
                efs_conn_destroy(nc);
                nc = p->conn[free_s];
            } else {
                p->conn[free_s] = nc;
            }
            p->fail_streak = 0;
            p->down_until_ms = 0;
            pthread_mutex_unlock(&p->lock);
            return nc;
        }

        p->busy[free_s] = 1;
        struct efs_conn *c = p->conn[free_s];
        pthread_mutex_unlock(&p->lock);
        return c;
    }
}

void server_peer_conn_release(const char *host, uint16_t port,
                              struct efs_conn *c)
{
    if (!c || !host)
        return;
    struct peer_slot *p = peer_slot_for(host, port);
    if (!p) {
        efs_conn_destroy(c);
        return;
    }
    pthread_mutex_lock(&p->lock);
    for (int s = 0; s < EFS_PEER_CONNS_PER_NODE; s++) {
        if (p->conn[s] == c) {
            p->busy[s] = 0;
            pthread_cond_signal(&p->cv);
            pthread_mutex_unlock(&p->lock);
            return;
        }
    }
    pthread_mutex_unlock(&p->lock);
    efs_conn_destroy(c);
}

void server_peer_conn_drop(const char *host, uint16_t port, struct efs_conn *c)
{
    if (!c)
        return;
    struct peer_slot *p = peer_slot_for(host, port);
    if (!p) {
        efs_conn_destroy(c);
        return;
    }
    pthread_mutex_lock(&p->lock);
    for (int s = 0; s < EFS_PEER_CONNS_PER_NODE; s++) {
        if (p->conn[s] == c) {
            p->conn[s] = NULL;
            p->busy[s] = 0;
            pthread_cond_signal(&p->cv);
            pthread_mutex_unlock(&p->lock);
            efs_conn_destroy(c);
            return;
        }
    }
    pthread_mutex_unlock(&p->lock);
    efs_conn_destroy(c);
}
