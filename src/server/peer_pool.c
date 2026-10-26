#include "server_internal.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>

/* Persistent server→server pool. Same idea as the client conn pool:
 * connect once per peer, upgrade to RDMA when the HCA is usable, reuse
 * across meta rebuild/flush/migrate. Drop on protocol/net errors so the
 * next checkout reconnects. */

/* Callers BLOCK on the condvar below once every connection to a peer is busy,
 * so this is the hard ceiling on server->server concurrency. At 4 it capped the
 * parallel metadata flush at 4-way no matter how many shard tables were fanned
 * out, which is most of why flushing 62 tables still took ~65 ms. */
#define EFS_PEER_DATA_CONNS 32
/* At most one coalesced remote proof per hosted metadata group. Do not let
 * forwarded mutations consume the connections needed to finish those reads. */
#define EFS_PEER_READ_CONNS 2
#define EFS_PEER_CONNS_PER_NODE (EFS_PEER_DATA_CONNS + EFS_PEER_READ_CONNS)
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
    uint64_t reaped_ms;     /* last idle scan (peer_reap_idle_locked) */
    uint32_t fail_streak;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    pthread_cond_t read_cv;
};

static uint64_t peer_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static struct peer_slot g_peers[EFS_MAX_NODES];
static pthread_mutex_t g_peers_mu = PTHREAD_MUTEX_INITIALIZER;
/* Read without g_peers_mu by peer_slot_for and server_peer_note_up, written
 * under it: atomic (TSan, F1 gate). */
static int g_peers_inited;

void server_peer_pool_init(void)
{
    pthread_mutex_lock(&g_peers_mu);
    if (__atomic_load_n(&g_peers_inited, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&g_peers_mu);
        return;
    }
    for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
        memset(&g_peers[i], 0, sizeof(g_peers[i]));
        pthread_mutex_init(&g_peers[i].lock, NULL);
        pthread_cond_init(&g_peers[i].cv, NULL);
        pthread_cond_init(&g_peers[i].read_cv, NULL);
    }
    __atomic_store_n(&g_peers_inited, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_peers_mu);
}

void server_peer_pool_shutdown(void)
{
    pthread_mutex_lock(&g_peers_mu);
    if (!__atomic_load_n(&g_peers_inited, __ATOMIC_ACQUIRE)) {
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
    __atomic_store_n(&g_peers_inited, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_peers_mu);
}

static struct peer_slot *peer_slot_for(const char *host, uint16_t port)
{
    if (!host || !*host || port == 0)
        return NULL;
    if (!__atomic_load_n(&g_peers_inited, __ATOMIC_ACQUIRE))
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

/* The heartbeat reached host:port. End a connect cooldown early: the
 * 10 s skip exists for peers that keep failing, and a restarted peer that
 * answers heartbeats was otherwise refused for the rest of it (a client
 * fsync after a member restart returned EIO). Stale pooled connections
 * are still found and dropped on use. Never creates a slot. */
void server_peer_note_up(const char *host, uint16_t port)
{
    struct peer_slot *p = NULL;

    if (!host || !*host || port == 0 || !__atomic_load_n(&g_peers_inited, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&g_peers_mu);
    for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
        if (g_peers[i].in_use && g_peers[i].port == port &&
            strcmp(g_peers[i].host, host) == 0) {
            p = &g_peers[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_peers_mu);
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    p->fail_streak = 0;
    p->down_until_ms = 0;
    pthread_mutex_unlock(&p->lock);
}

/* F1: the pool grew to its busiest moment and never shrank; every
 * connection holds a connection thread on the peer (stack, request
 * buffers). Close one that has been idle EFS_PEER_CONN_IDLE_MS: it has no
 * owner, and last_ok_ms is its last completed send or receive. At most
 * one scan a second per peer. Checkout takes the lowest free slot, so a
 * burst's extra slots go idle first. p->lock held. */
#define EFS_PEER_CONN_IDLE_MS 30000

static void peer_reap_idle_locked(struct peer_slot *p, uint64_t now)
{
    if (now - p->reaped_ms < 1000)
        return;
    p->reaped_ms = now;
    for (int s = 0; s < EFS_PEER_CONNS_PER_NODE; s++) {
        struct efs_conn *c = p->conn[s];
        if (!c || p->busy[s] || c->last_ok_ms <= 0 ||
            now - (uint64_t)c->last_ok_ms <= EFS_PEER_CONN_IDLE_MS)
            continue;
        efs_conn_destroy(c);
        p->conn[s] = NULL;
    }
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
    /* W125/W133: every daemon-to-daemon connection (HELLO join, Raft,
     * forwards, segments) declares the protocol like any client, before
     * the RDMA upgrade; the peer's HELLO/build-id gate follows. */
    if (efs_conn_declare(nc) != EFS_OK) {
        efs_conn_destroy(nc);
        return NULL;
    }
    if (efs_rdma_available() && efs_rdma_client_upgrade(nc) != 0) {
        if (efs_rdma_transport() == EFS_TRANSPORT_RDMA) {
            efs_conn_destroy(nc);
            return NULL;
        }
        /* auto: this connection stays TCP; say so (bounded). */
        static int noted;
        if (__sync_fetch_and_add(&noted, 1) < 5)
            fprintf(stderr, "efsd: transport auto: RDMA upgrade to %s:%u failed; "
                    "this peer connection uses tcp\n", host, port);
    }
    return nc;
}

/* A private connection outside the pool for a long-lived owner thread
 * (the per-peer Raft sender). Raft AppendEntries/votes used to check a
 * pool conn out per message; when forwarded client commands filled all
 * EFS_PEER_CONNS_PER_NODE slots, the senders and the cluster heartbeat
 * queued behind them in server_peer_conn_get, heartbeats stopped, the
 * group lost its leader, the forwards then waited for a leader and the
 * pool never drained (group 0 term 911 -> 1783 in 15 min, Sep 21).
 * Consensus traffic must not share a bounded pool with client work. */
struct efs_conn *server_peer_conn_new(const char *host, uint16_t port)
{
    struct in_addr a;
    if (!host || inet_pton(AF_INET, host, &a) != 1 || port == 0)
        return NULL;
    return peer_connect(host, port);
}

static struct efs_conn *peer_conn_get(const char *host, uint16_t port, int read)
{
    struct peer_slot *p = peer_slot_for(host, port);
    if (!p)
        return NULL;

    uint64_t now = peer_now_ms();
    int begin = read ? EFS_PEER_DATA_CONNS : 0;
    int end = read ? EFS_PEER_CONNS_PER_NODE : EFS_PEER_DATA_CONNS;
    pthread_cond_t *cv = read ? &p->read_cv : &p->cv;
    pthread_mutex_lock(&p->lock);
    /* Down-cooldown: don't even try to connect to a peer that has failed
     * repeatedly; the caller treats NULL as "peer down" and skips it fast. */
    if (p->down_until_ms > now) {
        pthread_mutex_unlock(&p->lock);
        return NULL;
    }
    peer_reap_idle_locked(p, now);
    for (;;) {
        if(!efs_net_remaining_ms(1)) {
            pthread_mutex_unlock(&p->lock);return NULL;
        }
        int free_s = -1;
        for (int s = begin; s < end; s++) {
            if (!p->busy[s]) {
                free_s = s;
                break;
            }
        }
        if (free_s < 0) {
            /* Bounded wait: never park a caller forever on a busy pool; after
             * the deadline let the caller fail/back off. */
            struct timespec ts;
            int wait_ms=efs_net_remaining_ms(5000);
            if(!wait_ms) { pthread_mutex_unlock(&p->lock);return NULL; }
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec+=wait_ms/1000;
            ts.tv_nsec+=(long)(wait_ms%1000)*1000000;
            if(ts.tv_nsec>=1000000000L) { ts.tv_sec++;ts.tv_nsec-=1000000000L; }
            int wrc = pthread_cond_timedwait(cv, &p->lock, &ts);
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
                pthread_cond_signal(cv);
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

struct efs_conn *server_peer_conn_get(const char *host, uint16_t port)
{
    return peer_conn_get(host, port, 0);
}

struct efs_conn *server_peer_read_conn_get(const char *host, uint16_t port)
{
    return peer_conn_get(host, port, 1);
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
            peer_reap_idle_locked(p, peer_now_ms());
            pthread_cond_signal(s >= EFS_PEER_DATA_CONNS ? &p->read_cv : &p->cv);
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
            pthread_cond_signal(s >= EFS_PEER_DATA_CONNS ? &p->read_cv : &p->cv);
            pthread_mutex_unlock(&p->lock);
            efs_conn_destroy(c);
            return;
        }
    }
    pthread_mutex_unlock(&p->lock);
    efs_conn_destroy(c);
}
