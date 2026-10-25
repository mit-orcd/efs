#include "../common/parse_host_port.h"
#include "client_internal.h"
#include "efs/placement.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include "efs/member_view.h"
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
/* While a peer is in that cooldown, one detached thread per peer retries a
 * plain TCP connect at most this often and ends the cooldown when it
 * succeeds. Callers never wait on it, so a blackholed peer still costs no
 * foreground connect timeout, but a restarted peer is used again within
 * about a second instead of after the rest of the 30 s (a create's fsync
 * returned EIO for that remainder whenever placement included the peer). */
#define EFS_NODE_PROBE_MS   1000

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static int parse_host_port(const char *str, char *host, size_t host_len, uint16_t *port)
{
    return efs_parse_host_port(str, host, host_len, port);
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
    /* Default 64. It was 16 when EFS_SERVER_MAX_CONNS was 512 (9 clients
     * × 256 × 4 nodes overflowed accept()); the server cap is 4096 now,
     * and 13 clients × 64 is 832 conns per server. Every checkout holds
     * one conn for the whole RPC, a PUT holds three, so 400 ecopy
     * threads on 16 conns queued metadata RPCs behind chunk PUTs for the
     * full 5 s checkout timeout (Sep 30 2026). Slots connect lazily; one
     * RDMA conn pins 4 × 88 KiB + 32 × 84 KiB ≈ 3.0 MB on each end. */
    int n = 64;
    if (env && *env) {
        int v = atoi(env);
        if (v >= 1 && v <= EFS_CLIENT_CONNS_PER_NODE)
            n = v;
    }
    g_client.conn_pool_size = n;

    for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
        for (int s = 0; s < EFS_CLIENT_CONNS_PER_NODE; s++) {
            /* A checkout owns this slot. Destroying it here frees the
             * object while the owner still sends, then a new wrap can
             * recycle the same fd number under a live dest_qpn. */
            if (conn_pool_inited && g_client.conn_busy[i][s])
                continue;
            /* First init: BSS zeros are NULL — do not destroy. */
            if (conn_pool_inited && g_client.conn[i][s])
                efs_conn_destroy(g_client.conn[i][s]);
            g_client.conn[i][s] = NULL;
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

static int slot_for_conn(uint32_t idx, struct efs_conn *conn)
{
    int n = pool_size();
    for (int s = 0; s < n; s++) {
        if (g_client.conn[idx][s] == conn)
            return s;
    }
    return -1;
}

/* Map placement / protocol node id → index in g_client.nodes[]. */
static int node_index_for_id(efs_node_id_t node_id)
{
    if (node_id == 0)
        return -1;
    /* K33: the live view appends rows and then publishes the count with
     * release order (efs_member_view_publish); rows below it never change. */
    uint32_t n = __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) {
        if (g_client.nodes[i].id == node_id)
            return (int)i;
    }
    return -1;
}

/* K33 live client view (efs/member_view.h): the mount installs the committed
 * record it mounted with; one thread asks a member for its record every
 * EFS_VIEW_POLL_MS (rotating over the members) and installs a later
 * revision. A request for a node id the table lacks (a chunk another client
 * wrote on an added member) wakes it and waits up to EFS_VIEW_AWAIT_MS for
 * a round that asks every member. Peer connections to added members then
 * open on demand through the ordinary pool (TCP, RDMA upgrade). */
#define EFS_VIEW_POLL_MS   1000 /* EFS_CLIENT_VIEW_POLL_MS overrides (50..60000) */
#define EFS_VIEW_AWAIT_MS  2000
#define EFS_VIEW_MISS_MS   1000
#define EFS_VIEW_REFUSE_LOG_MS 30000

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct efs_membership cur; /* the installed record */
    int have, running, stop, kick;
    uint64_t rounds;           /* completed poll rounds */
    uint32_t next;             /* rotating member index */
    int poll_ms;
    int64_t miss_until[EFS_MAX_NODES + 1];
    int last_refusal;
    efs_node_id_t last_refusal_node;
    int64_t last_refusal_ms;
    uint64_t installs, refusals;
    pthread_t th;
} g_view = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
static __thread int view_thread_self;

static int view_reply(int fd, struct efs_conn *c, struct efs_membership *out)
{
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t len = 0;
    int rc;
    if (c)
        rc = efs_conn_send_msg(c, EFS_MSG_MEMBERSHIP_READ, NULL, 0) == 0 &&
             efs_conn_recv_msg(c, &type, &payload, &len) == 0;
    else
        rc = efs_send_msg(fd, EFS_MSG_MEMBERSHIP_READ, NULL, 0) == 0 &&
             efs_recv_msg(fd, &type, &payload, &len) == 0;
    if (!rc) {
        free(payload);
        return EFS_ERR_NET;
    }
    rc = type == EFS_MSG_MEMBERSHIP_READ_REPLY ? efs_member_view_parse_reply(payload, len, out)
                                               : EFS_ERR_PROTO;
    free(payload);
    return rc;
}

static void view_note_local(uint32_t from, uint32_t to)
{
    if (g_client.local_node_id)
        return;
    for (uint32_t i = from; i < to; i++)
        if (resolve_local(g_client.nodes[i].addr)) {
            g_client.local_node_id = g_client.nodes[i].id;
            return;
        }
}

uint32_t efs_client_view_revision(void)
{
    pthread_mutex_lock(&g_view.mu);
    uint32_t r = g_view.have ? efs_member_view_revision(&g_view.cur) : 0;
    pthread_mutex_unlock(&g_view.mu);
    return r;
}

void efs_client_view_counters(uint64_t *installs, uint64_t *refusals)
{
    pthread_mutex_lock(&g_view.mu);
    if (installs)
        *installs = g_view.installs;
    if (refusals)
        *refusals = g_view.refusals;
    pthread_mutex_unlock(&g_view.mu);
}

int efs_client_view_mount(void)
{
    char err[256] = "";
    int last = EFS_ERR_NET;
    for (int attempt = 0; attempt < 40; attempt++) {
        uint32_t n = __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE);
        for (uint32_t i = 0; i < n; i++) {
            struct efs_membership m;
            int fd = efs_connect_declared(g_client.nodes[i].addr, g_client.nodes[i].port);
            if (fd < 0)
                continue;
            efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
            efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
            last = view_reply(fd, NULL, &m);
            close(fd);
            if (last != EFS_OK)
                continue;
            pthread_mutex_lock(&g_client.lock);
            uint32_t old = g_client.node_count;
            last = efs_member_view_publish(g_client.nodes, &g_client.node_count, &m, err,
                                           sizeof(err));
            uint32_t now = g_client.node_count;
            pthread_mutex_unlock(&g_client.lock);
            if (last != EFS_OK) {
                fprintf(stderr, "membership view: refused to mount: the node table does not "
                        "match the committed record of node %u: %s\n", g_client.nodes[i].id,
                        err);
                return last;
            }
            view_note_local(old, now);
            pthread_mutex_lock(&g_view.mu);
            g_view.cur = m;
            g_view.have = 1;
            pthread_mutex_unlock(&g_view.mu);
            printf("membership view: revision %u, %u members, cluster %016llx (from node %u)\n",
                   efs_member_view_revision(&m), m.count, (unsigned long long)m.cluster,
                   g_client.nodes[i].id);
            return EFS_OK;
        }
        usleep(250000);
    }
    fprintf(stderr, "membership view: no member returned the committed membership record "
            "(rc=%d)\n", last);
    return last;
}

/* g_view.mu held. Log a refusal once per (verdict, node) and then at most
 * every EFS_VIEW_REFUSE_LOG_MS. */
static void view_refused_locked(int verdict, efs_node_id_t from, const struct efs_membership *m,
                                const char *detail)
{
    int64_t now = monotonic_ms();
    g_view.refusals++;
    if (verdict == g_view.last_refusal && from == g_view.last_refusal_node &&
        now - g_view.last_refusal_ms < EFS_VIEW_REFUSE_LOG_MS)
        return;
    g_view.last_refusal = verdict;
    g_view.last_refusal_node = from;
    g_view.last_refusal_ms = now;
    fprintf(stderr, "membership view: refused the record from node %u: %s (its revision %u, "
            "%u members, cluster %016llx; installed revision %u, %u members, cluster %016llx)"
            "%s%s\n", from, efs_view_verdict_name(verdict), efs_member_view_revision(m),
            m->count, (unsigned long long)m->cluster, efs_member_view_revision(&g_view.cur),
            g_view.cur.count, (unsigned long long)g_view.cur.cluster, detail ? ": " : "",
            detail ? detail : "");
}

/* Judge one member's record and install it when it is a later revision.
 * 1 installed, 0 otherwise. Only the view thread installs after the mount,
 * so g_view.cur cannot change between the judgement and the install. The
 * table is extended without g_client.lock: rows below the old count are not
 * written and the count is published last (a FUSE thread that holds the
 * lock and waits in view_await would otherwise stall the install). */
static int view_consider(efs_node_id_t from, const struct efs_membership *m)
{
    char err[256] = "";
    pthread_mutex_lock(&g_view.mu);
    int verdict = efs_member_view_judge(&g_view.cur, m);
    uint32_t r0 = efs_member_view_revision(&g_view.cur);
    if (verdict != EFS_VIEW_ADVANCE) {
        /* An older revision is a member that has not learned yet: expected
         * for a moment after a commit, so not a refusal. */
        if (verdict != EFS_VIEW_SAME && verdict != EFS_VIEW_OLDER)
            view_refused_locked(verdict, from, m, NULL);
        pthread_mutex_unlock(&g_view.mu);
        return 0;
    }
    pthread_mutex_unlock(&g_view.mu);
    uint32_t old = __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE);
    int rc = efs_member_view_publish(g_client.nodes, &g_client.node_count, m, err, sizeof(err));
    uint32_t now = __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&g_view.mu);
    if (rc != EFS_OK) {
        view_refused_locked(EFS_VIEW_DIVERGENT, from, m, err);
        pthread_mutex_unlock(&g_view.mu);
        return 0;
    }
    g_view.cur = *m;
    g_view.installs++;
    pthread_cond_broadcast(&g_view.cv);
    pthread_mutex_unlock(&g_view.mu);
    view_note_local(old, now);
    for (uint32_t i = old; i < now; i++)
        printf("membership view: revision %u -> %u from node %u: added member %u at %s:%u "
               "(%u members, no remount)\n", r0, efs_member_view_revision(m), from,
               g_client.nodes[i].id, g_client.nodes[i].addr, g_client.nodes[i].port, now);
    fflush(stdout);
    return 1;
}

/* One round: ask the next member (all members when all is set) until one
 * gives a later revision. */
static void view_round(int all)
{
    uint32_t n = __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t idx;
        pthread_mutex_lock(&g_view.mu);
        idx = g_view.next++ % n;
        pthread_mutex_unlock(&g_view.mu);
        efs_node_id_t id = g_client.nodes[idx].id;
        if (efs_client_node_is_down(id))
            continue;
        struct efs_conn *c = efs_client_conn_get(id);
        if (!c)
            continue;
        struct efs_membership m;
        int rc = view_reply(-1, c, &m);
        if (rc == EFS_ERR_NET || rc == EFS_ERR_PROTO || rc == EFS_ERR_DECODE)
            efs_client_conn_drop(id, c);
        else
            efs_client_conn_release(id, c);
        if (rc == EFS_OK) {
            if (view_consider(id, &m) || !all)
                return;
        } else if (rc != EFS_ERR_UNFORMED) {
            pthread_mutex_lock(&g_view.mu);
            if (rc == EFS_ERR_DECODE || rc == EFS_ERR_UNSUPPORTED)
                view_refused_locked(EFS_VIEW_INVALID, id, &g_view.cur, "undecodable reply");
            pthread_mutex_unlock(&g_view.mu);
        }
        if (!all)
            return;
    }
}

static void *view_thread(void *arg)
{
    (void)arg;
    view_thread_self = 1;
    pthread_mutex_lock(&g_view.mu);
    while (!g_view.stop) {
        if (!g_view.kick) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += g_view.poll_ms / 1000;
            ts.tv_nsec += (long)(g_view.poll_ms % 1000) * 1000000;
            ts.tv_sec += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
            pthread_cond_timedwait(&g_view.cv, &g_view.mu, &ts);
            if (g_view.stop)
                break;
        }
        int all = g_view.kick;
        g_view.kick = 0;
        pthread_mutex_unlock(&g_view.mu);
        view_round(all);
        pthread_mutex_lock(&g_view.mu);
        g_view.rounds++;
        pthread_cond_broadcast(&g_view.cv);
    }
    pthread_mutex_unlock(&g_view.mu);
    return NULL;
}

int efs_client_view_start(void)
{
    pthread_mutex_lock(&g_view.mu);
    if (!g_view.have || g_view.running) {
        pthread_mutex_unlock(&g_view.mu);
        return g_view.have ? EFS_OK : EFS_ERR_INVAL;
    }
    g_view.stop = 0;
    g_view.poll_ms = EFS_VIEW_POLL_MS;
    const char *pe = getenv("EFS_CLIENT_VIEW_POLL_MS");
    if (pe && atoi(pe) >= 50 && atoi(pe) <= 60000)
        g_view.poll_ms = atoi(pe);
    if (pthread_create(&g_view.th, NULL, view_thread, NULL) != 0) {
        pthread_mutex_unlock(&g_view.mu);
        fprintf(stderr, "membership view: refresh thread not started; members added after "
                "this mount are used only after a remount\n");
        return EFS_ERR_NOMEM;
    }
    g_view.running = 1;
    pthread_mutex_unlock(&g_view.mu);
    return EFS_OK;
}

void efs_client_view_stop(void)
{
    pthread_mutex_lock(&g_view.mu);
    if (!g_view.running) {
        pthread_mutex_unlock(&g_view.mu);
        return;
    }
    g_view.stop = 1;
    pthread_cond_broadcast(&g_view.cv);
    pthread_mutex_unlock(&g_view.mu);
    pthread_join(g_view.th, NULL);
    pthread_mutex_lock(&g_view.mu);
    g_view.running = 0;
    pthread_cond_broadcast(&g_view.cv);
    pthread_mutex_unlock(&g_view.mu);
}

/* A node id the table lacks: wake the view thread and wait (bounded by the
 * caller's RPC deadline) for a round over every member that started after
 * this call. 1 when the id is a member now. */
static int view_await(efs_node_id_t id)
{
    if (id == 0 || id > EFS_MAX_NODES || view_thread_self)
        return 0;
    int64_t now = monotonic_ms(), end = now + EFS_VIEW_AWAIT_MS;
    uint64_t outer = efs_client_rpc_deadline_ms();
    if (outer && (int64_t)outer < end)
        end = (int64_t)outer;
    pthread_mutex_lock(&g_view.mu);
    if (!g_view.running || g_view.stop || now < g_view.miss_until[id]) {
        pthread_mutex_unlock(&g_view.mu);
        return 0;
    }
    uint64_t want = g_view.rounds + 2;
    g_view.kick = 1;
    pthread_cond_broadcast(&g_view.cv);
    int found = 0;
    while (!(found = node_index_for_id(id) >= 0) && g_view.running && !g_view.stop &&
           g_view.rounds < want && (now = monotonic_ms()) < end) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t w = end - now > 100 ? 100 : end - now;
        ts.tv_nsec += (long)w * 1000000;
        ts.tv_sec += ts.tv_nsec / 1000000000;
        ts.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&g_view.cv, &g_view.mu, &ts);
    }
    if (!found)
        g_view.miss_until[id] = monotonic_ms() + EFS_VIEW_MISS_MS;
    pthread_mutex_unlock(&g_view.mu);
    return found;
}

static int node_index_or_await(efs_node_id_t id)
{
    int idx = node_index_for_id(id);
    if (idx < 0 && view_await(id))
        idx = node_index_for_id(id);
    return idx;
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

static void node_probe_maybe_start(int idx, int64_t now);

int efs_client_node_is_down(efs_node_id_t node_id)
{
    int idx = node_index_or_await(node_id);
    if (idx < 0)
        return 1;
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int64_t now = monotonic_ms();
    int down = g_client.node_down_until_ms[idx] > now;
    /* K35: PUT placement asks here and never checks out a connection to a
     * peer in cooldown, so the reprobe must start here too, or a returned
     * peer took no fragment for the rest of the 30 s (degraded stripes). */
    if (down)
        node_probe_maybe_start(idx, now);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
    return down;
}

int efs_client_node_reprobe(efs_node_id_t node_id)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0)
        return 0;
    char host[64] = {0};
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int down = g_client.node_down_until_ms[idx] > monotonic_ms();
    uint16_t port = g_client.nodes[idx].port;
    strncpy(host, g_client.nodes[idx].addr, sizeof(host) - 1);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
    if (!down)
        return 1;
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return 0;
    close(fd);
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    g_client.node_down_until_ms[idx] = 0;
    g_client.node_fail_streak[idx] = 0;
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
    return 1;
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
    uint64_t deadline = efs_client_rpc_deadline_ms();
    if (deadline && now >= (int64_t)deadline)
        return; /* send/receive cancellation is not a peer-health verdict */
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    now = monotonic_ms();
    if (deadline && now >= (int64_t)deadline) {
        pthread_mutex_unlock(&g_client.conn_lock[idx]);
        return;
    }
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
static int conn_fd_is_dead(struct efs_conn *c)
{
    if (!c || c->fd < 0)
        return 1;
    /* Recycled fd number: the new socket can be ESTABLISHED. */
    if (!efs_conn_fd_matches(c))
        return 1;
    int fd = c->fd;
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

struct node_probe {
    int idx;
    char host[64];
    uint16_t port;
};

static void *node_probe_run(void *arg)
{
    struct node_probe *p = arg;
    int fd = efs_connect_tcp(p->host, p->port);
    if (fd >= 0)
        close(fd);
    pthread_mutex_lock(&g_client.conn_lock[p->idx]);
    if (fd >= 0 && g_client.node_down_until_ms[p->idx] > 0) {
        /* Listening again: let the next checkout make a real connection.
         * A peer that accepts but cannot serve fails that checkout and
         * re-enters the cooldown through the usual failure streak. */
        g_client.node_down_until_ms[p->idx] = 0;
        g_client.node_fail_streak[p->idx] = 0;
    }
    g_client.node_probe_inflight[p->idx] = 0;
    pthread_cond_broadcast(&g_client.conn_cv[p->idx]);
    pthread_mutex_unlock(&g_client.conn_lock[p->idx]);
    free(p);
    return NULL;
}

/* conn_lock[idx] held, peer in cooldown: start at most one background
 * reprobe per EFS_NODE_PROBE_MS. Best effort; a failed start just waits. */
static void node_probe_maybe_start(int idx, int64_t now)
{
    if (g_client.node_probe_inflight[idx] ||
        now < g_client.node_probe_after_ms[idx])
        return;
    struct node_probe *p = calloc(1, sizeof(*p));
    if (!p)
        return;
    p->idx = idx;
    p->port = g_client.nodes[idx].port;
    strncpy(p->host, g_client.nodes[idx].addr, sizeof(p->host) - 1);
    g_client.node_probe_after_ms[idx] = now + EFS_NODE_PROBE_MS;
    pthread_attr_t attr;
    pthread_t t;
    if (pthread_attr_init(&attr) != 0) {
        free(p);
        return;
    }
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, node_probe_run, p) == 0)
        g_client.node_probe_inflight[idx] = 1;
    else
        free(p);
    pthread_attr_destroy(&attr);
}

struct efs_conn *efs_client_conn_get(efs_node_id_t node_id)
{
    int idx = node_index_or_await(node_id);
    if (idx < 0)
        return NULL;
    int n = pool_size();

    int64_t admission_end = monotonic_ms() + 5000;
    uint64_t outer = efs_client_rpc_deadline_ms();
    if (outer && outer < (uint64_t)admission_end)
        admission_end = (int64_t)outer;
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int64_t now = monotonic_ms();
    if (g_client.node_down_until_ms[idx] > now) {
        node_probe_maybe_start(idx, now);
        pthread_mutex_unlock(&g_client.conn_lock[idx]);
        return NULL;
    }
    /* Cooldown expired — clear streak so a recovered peer gets a clean probe. */
    if (g_client.node_down_until_ms[idx] > 0) {
        g_client.node_down_until_ms[idx] = 0;
        g_client.node_fail_streak[idx] = 0;
    }

    for (;;) {
        now = monotonic_ms();
        if (now >= admission_end) {
            pthread_mutex_unlock(&g_client.conn_lock[idx]);
            return NULL;
        }
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
            int64_t wait = admission_end - now;
            if (wait > 50) wait = 50;
            ts.tv_nsec += (long)wait * 1000000;
            ts.tv_sec += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
            int wrc = pthread_cond_timedwait(&g_client.conn_cv[idx],
                                             &g_client.conn_lock[idx], &ts);
            if ((wrc && wrc != ETIMEDOUT) || monotonic_ms() >= admission_end) {
                /* Silent until Sep 30 2026: 244 RPCs at exactly 5.0 s
                 * (and 10, 15) in one ecopy were this, not the network.
                 * Say so, rate-limited, with the pool size. */
                static int timeouts;
                int k = __sync_fetch_and_add(&timeouts, 1);

                pthread_mutex_unlock(&g_client.conn_lock[idx]);
                if (k < 10 || (k % 100) == 0)
                    fprintf(stderr,
                            "efs: conn pool node=%u checkout deadline reached "
                            "(pool=%d per node, EFS_CLIENT_CONNS_PER_NODE; "
                            "timeout #%d)\n",
                            (unsigned)node_id, n, k + 1);
                return NULL;
            }
            continue;
        }

        if (!g_client.conn[idx][free_slot]) {
            /* Connect outside the lock: a blackholed peer's connect timeout
             * must not stall every other checkout for this node. Reserve the
             * slot (busy, conn=NULL) so waiters block on the cond instead. */
            char host[64];
            uint16_t port = g_client.nodes[idx].port;
            strncpy(host, g_client.nodes[idx].addr, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            g_client.conn_busy[idx][free_slot] = 1;
            pthread_mutex_unlock(&g_client.conn_lock[idx]);

            /* The checkout limit includes DNS, TCP connect and optional
             * upgrade. Network helpers must not start fresh local budgets. */
            uint64_t previous = efs_client_rpc_deadline_ms();
            efs_client_rpc_set_deadline_ms((uint64_t)admission_end);
            int fd = efs_connect_tcp(host, port);
            int local_failure = fd < 0 &&
                (errno == ENOMEM || errno == ENOBUFS || errno == EAGAIN ||
                 errno == EMFILE || errno == ENFILE);
            struct efs_conn *nc = NULL;
            if (fd >= 0) {
                efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
                efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
                nc = efs_conn_wrap_tcp(fd, 0);
                if (!nc) {
                    local_failure = 1; /* wrapper allocation, not peer failure */
                    close(fd);
                }
                /* W125/W133: declare the protocol before anything else, the
                 * RDMA upgrade included (the server serves nothing before an
                 * accepted declaration); a server that does not accept it is
                 * never used. */
                if (nc) {
                    int drc = efs_conn_declare(nc);
                    if (drc != EFS_OK) {
                        static int warned;
                        if (__sync_fetch_and_add(&warned, 1) < 5)
                            fprintf(stderr, "efs-fuse: node %u refused or did not answer the "
                                    "protocol %u declaration (rc=%d): incompatible server\n",
                                    node_id, EFS_PROTOCOL_VERSION, drc);
                        efs_conn_destroy(nc);
                        nc = NULL;
                    }
                }
                if (nc && efs_rdma_available() &&
                    efs_rdma_client_upgrade(nc) != 0) {
                    if (efs_rdma_transport() == EFS_TRANSPORT_RDMA) {
                        /* Strict mode: no silent TCP fallback. */
                        efs_conn_destroy(nc);
                        nc = NULL;
                    } else {
                        /* auto: this connection stays TCP; say so (bounded). */
                        static int noted;
                        if (__sync_fetch_and_add(&noted, 1) < 5)
                            fprintf(stderr, "efs-fuse: transport auto: RDMA upgrade to node %u "
                                    "failed; this connection uses tcp\n", node_id);
                    }
                }
            }
            efs_client_rpc_set_deadline_ms(previous);
            if (!nc) {
                pthread_mutex_lock(&g_client.conn_lock[idx]);
                g_client.conn_busy[idx][free_slot] = 0;
                now = monotonic_ms();
                /* Caller or local checkout expiry is cancellation, not a
                 * peer-health verdict. Count genuine failures within budget. */
                if (!local_failure && now < admission_end) {
                    if (g_client.node_fail_streak[idx] < 1000)
                        g_client.node_fail_streak[idx]++;
                    if (g_client.node_fail_streak[idx] >= EFS_NODE_DOWN_FAILS &&
                        g_client.node_down_until_ms[idx] <= now)
                        g_client.node_down_until_ms[idx] = now + EFS_NODE_DOWN_MS;
                }
                pthread_cond_signal(&g_client.conn_cv[idx]);
                pthread_mutex_unlock(&g_client.conn_lock[idx]);
                return NULL;
            }

            pthread_mutex_lock(&g_client.conn_lock[idx]);
            /* We reserved this empty slot (busy=1, conn=NULL). Install
             * ours. Stealing a pointer another thread already returned
             * would double-checkout one QP (the old "keep existing" path). */
            if (monotonic_ms() >= admission_end) {
                efs_conn_destroy(nc);
                g_client.conn_busy[idx][free_slot] = 0;
                pthread_cond_signal(&g_client.conn_cv[idx]);
                pthread_mutex_unlock(&g_client.conn_lock[idx]);
                return NULL;
            }
            g_client.conn[idx][free_slot] = nc;
            g_client.node_fail_streak[idx] = 0;
            g_client.node_down_until_ms[idx] = 0;
            efs_conn_bind_gen(nc, &g_client.conn_gen[idx][free_slot]);
            efs_conn_note_ok(nc);
            /* busy already set */
            pthread_mutex_unlock(&g_client.conn_lock[idx]);
            return nc;
        }

        struct efs_conn *c = g_client.conn[idx][free_slot];
        /* A conn that completed an RPC in the last second is not in
         * CLOSE-WAIT. The probe stays for one that sat idle. */
        if (c->last_ok_ms == 0 || now - c->last_ok_ms > 1000) {
            if (conn_fd_is_dead(c)) {
                efs_conn_destroy(c);
                g_client.conn[idx][free_slot] = NULL;
                continue;
            }
            efs_conn_note_ok(c);
        }
        if (!c->use_gen)
            efs_conn_bind_gen(c, &g_client.conn_gen[idx][free_slot]);
        g_client.conn_busy[idx][free_slot] = 1;
        pthread_mutex_unlock(&g_client.conn_lock[idx]);
        return c;
    }
}

void efs_client_conn_release(efs_node_id_t node_id, struct efs_conn *conn)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0 || !conn)
        return;
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int s = slot_for_conn((uint32_t)idx, conn);
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
        if (g_client.conn[idx][s]) {
            efs_conn_destroy(g_client.conn[idx][s]);
            g_client.conn[idx][s] = NULL;
        }
    }
    pthread_cond_broadcast(&g_client.conn_cv[idx]);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

void efs_client_conn_drop(efs_node_id_t node_id, struct efs_conn *conn)
{
    int idx = node_index_for_id(node_id);
    if (idx < 0 || !conn)
        return;
    int n = pool_size();
    pthread_mutex_lock(&g_client.conn_lock[idx]);
    int s = slot_for_conn((uint32_t)idx, conn);
    if (s >= 0) {
        if (g_client.conn[idx][s])
            efs_conn_destroy(g_client.conn[idx][s]);
        g_client.conn[idx][s] = NULL;
        g_client.conn_busy[idx][s] = 0;
    } else {
        efs_conn_destroy(conn);
    }
    /* After efsd restart the whole idle pool for this peer is usually dead;
     * flush it so the next checkout opens fresh connections. */
    for (int i = 0; i < n; i++) {
        if (g_client.conn_busy[idx][i])
            continue;
        if (g_client.conn[idx][i]) {
            efs_conn_destroy(g_client.conn[idx][i]);
            g_client.conn[idx][i] = NULL;
        }
    }
    pthread_cond_broadcast(&g_client.conn_cv[idx]);
    pthread_mutex_unlock(&g_client.conn_lock[idx]);
}

static char g_shutdown_reason[192];

const char *efs_client_shutdown_reason(void)
{
    return g_shutdown_reason;
}

int efs_client_shutdown(void)
{
    static int done, result;
    if (done)
        return result;
    done = 1;

    /* Quiesce the reclaim pool first: its in-flight GET/PUT would otherwise
     * race the connection teardown below. */
    efs_dcache_reclaim_stop();
    /* Persist any write-combined chunks before tearing down connections. */
    int flush_rc = efs_dcache_flush_all();

    /* Stop the flush thread before freeing state it might be using; join
     * blocks until any in-flight flush completes. */
    efs_client_stop_meta_flush();
    /* Settle what this mount can, then revoke its session (D31, always:
     * D42): the leaders drain and collect what is left. Needs the
     * connections. A failure is reported, never a clean completion. */
    efs_client_stop_phase("settling", 0, 0);
    int settle_rc = efs_client_put_settle();
    efs_client_stop_phase("revoking", 0, 0);
    int revoke_rc = efs_client_session_end();
    if (flush_rc != EFS_OK || settle_rc != EFS_OK || revoke_rc != EFS_OK) {
        fprintf(stderr, "efs: unmount not clean: flush rc=%d settle rc=%d revoke rc=%d\n",
                flush_rc, settle_rc, revoke_rc);
        result = flush_rc != EFS_OK ? flush_rc : settle_rc != EFS_OK ? settle_rc : revoke_rc;
        if (flush_rc != EFS_OK)
            snprintf(g_shutdown_reason, sizeof(g_shutdown_reason), "data flush rc=%d", flush_rc);
        else if (settle_rc != EFS_OK)
            snprintf(g_shutdown_reason, sizeof(g_shutdown_reason), "put settle rc=%d", settle_rc);
        else
            snprintf(g_shutdown_reason, sizeof(g_shutdown_reason), "%s",
                     efs_client_session_end_reason());
    }

    /* Stop the staging-table evictor (it takes the table locks; must be
     * gone before the export is freed). */
    efs_client_stage_evict_stop();

    /* Join the read pools after the flush: a flush can read (merge-base
     * GET), and worker exit is what frees the per-thread decode scratch. */
    efs_client_read_pools_stop();

    if (conn_pool_inited) {
        for (uint32_t i = 0; i < EFS_MAX_NODES; i++) {
            pthread_mutex_lock(&g_client.conn_lock[i]);
            for (int s = 0; s < EFS_CLIENT_CONNS_PER_NODE; s++) {
                if (g_client.conn[i][s]) {
                    efs_conn_destroy(g_client.conn[i][s]);
                    g_client.conn[i][s] = NULL;
                }
                g_client.conn_busy[i][s] = 0;
            }
            pthread_mutex_unlock(&g_client.conn_lock[i]);
            pthread_cond_destroy(&g_client.conn_cv[i]);
            pthread_mutex_destroy(&g_client.conn_lock[i]);
        }
        conn_pool_inited = 0;
    }

    efs_client_dirty_sets_free();
    efs_export_free(&g_client.export);
    pthread_mutex_destroy(&g_client.lock);
    if (g_client.dir_locks_ready) {
        for (int i = 0; i < EFS_DIR_LOCKS; i++)
            pthread_mutex_destroy(&g_client.dir_lock[i]);
        pthread_mutex_destroy(&g_client.dirty_mu);
        pthread_mutex_destroy(&g_client.idx_mu);
        g_client.dir_locks_ready = 0;
    }
    return result;
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
    /* W133: LIST_NODES is served only on a declared connection. */
    if (efs_fd_declare(fd) != EFS_OK) {
        close(fd);
        return -1;
    }

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
