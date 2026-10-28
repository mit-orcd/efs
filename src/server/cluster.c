#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "efs/fault_exit.h"

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
    /* K33 (M2): this store's token, its cluster and whether it is formed. */
    h.token = s->mstore.token;
    {
        struct efs_membership m;
        int formed = server_membership_get(&m);
        h.cluster = formed ? m.cluster : s->mstore.cluster;
        h.flags = formed ? EFS_HELLO_F_FORMED : 0;
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
        if (ack->reject_reason == EFS_HELLO_REJECT_PENDING) {
            static uint64_t last_note;
            uint64_t now = (uint64_t)time(NULL);
            if (now - last_note >= 10) {
                last_note = now;
                fprintf(stderr,
                        "Join pending at %s:%u — it has not learned this node's membership "
                        "yet (%u committed members there); node %llu at %s:%u (store "
                        "%016llx) retrying\n", peer_host, peer_port, ack->node_count,
                        (unsigned long long)s->id, s->addr, s->port,
                        (unsigned long long)s->mstore.token);
            }
            free(payload);
            server_peer_conn_release(peer_host, peer_port, pc);
            return EFS_JOIN_PENDING;
        }
        if (ack->reject_reason == EFS_HELLO_REJECT_FORMED) {
            fprintf(stderr,
                    "Join refused by %s:%u — node %llu at %s:%u (store %016llx) can never "
                    "be a member of this cluster (%u committed members): its node id or "
                    "endpoint belongs to another store, or this store belongs to another "
                    "cluster. A member keeps its node id, endpoint and store.\n",
                    peer_host, peer_port, (unsigned long long)s->id, s->addr, s->port,
                    (unsigned long long)s->mstore.token, ack->node_count);
            free(payload);
            server_peer_conn_drop(peer_host, peer_port, pc);
            return EFS_JOIN_REFUSED_FORMED;
        }
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
    /* W153: once formed, only committed members enter the table. */
    s->node_count = server_member_filter(s->nodes, s->node_count);
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
        local->token = s->mstore.token;
    }
    server_dedupe_nodes_locked(s);
    server_heartbeat_apply_locked(s);
    joined = s->node_count;
    server_nodes_mark_dirty(s);
    pthread_mutex_unlock(&s->lock);
    server_nodes_flush_dirty(s);
    free(payload);
    server_peer_conn_release(peer_host, peer_port, pc);

    printf("Joined cluster with %u nodes\n", joined);
    return 0;
}

/* K33 (M4', M5): one HELLO of a joining store (node id 0). See
 * server_internal.h. A promise or a committed record is persisted before
 * this returns EFS_JOIN_ADMITTED; nothing else is written. */
int server_join_as_new(struct efsd_server *s, const char *peer_host, uint16_t peer_port,
                       struct efs_join_note *note)
{
    struct efs_membership m;
    char err[1024];
    int rc = -1;

    memset(note, 0, sizeof(*note));
    struct efs_conn *pc = server_peer_conn_new(peer_host, peer_port);
    if (!pc)
        return -1;
    struct efs_msg_hello h;
    memset(&h, 0, sizeof(h));
    h.version = EFS_VERSION_PACK;
    strncpy(h.build_id, EFS_BUILD_ID, sizeof(h.build_id) - 1);
    h.node_id = 0;
    strncpy(h.addr, s->addr, sizeof(h.addr) - 1);
    h.port = s->port;
    server_format_storage_paths(s, h.storage_path, sizeof(h.storage_path));
    h.quota = s->quota;
    h.token = s->mstore.token;
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_conn_send_msg(pc, EFS_MSG_HELLO, &h, sizeof(h)) != 0 ||
        efs_conn_recv_msg(pc, &type, &payload, &payload_len) != 0 ||
        type != EFS_MSG_HELLO_ACK || payload_len != sizeof(struct efs_msg_hello_ack)) {
        free(payload);
        efs_conn_destroy(pc);
        return -1;
    }
    efs_conn_destroy(pc);
    struct efs_msg_hello_ack *ack = payload;
    note->cluster = ack->cluster;
    note->count = ack->node_count;
    note->formed = ack->formed;
    for (uint32_t i = 0; i < ack->node_count && i < EFS_MAX_NODES && note->npeers < EFS_MAX_NODES;
         i++) {
        if (!ack->nodes[i].port || !ack->nodes[i].addr[0] ||
            (ack->nodes[i].port == s->port && !strcmp(ack->nodes[i].addr, s->addr)))
            continue;
        snprintf(note->peer[note->npeers].addr, sizeof(note->peer[0].addr), "%.*s",
                 (int)sizeof(ack->nodes[i].addr) - 1, ack->nodes[i].addr);
        note->peer[note->npeers++].port = ack->nodes[i].port;
    }
    if (ack->reject_reason == EFS_HELLO_REJECT_VERSION) {
        fprintf(stderr, "Join refused by %s:%u — VERSION MISMATCH: this node runs build '%s', "
                "the cluster runs build '%.*s'. Run the same efsd build on every node.\n",
                peer_host, peer_port, EFS_BUILD_ID, (int)sizeof(ack->build_id), ack->build_id);
    } else if (ack->reject_reason == EFS_HELLO_REJECT_PENDING) {
        rc = EFS_JOIN_PENDING;
    } else if (ack->reject_reason == EFS_HELLO_REJECT_FORMED) {
        fprintf(stderr, "Join refused by %s:%u (cluster %016llx) — this store (%016llx) at "
                "%s:%u can never be a member: its endpoint belongs to another member's store, "
                "or this store is a member at another endpoint.\n", peer_host, peer_port,
                (unsigned long long)ack->cluster, (unsigned long long)s->mstore.token, s->addr,
                s->port);
        rc = EFS_JOIN_REFUSED_FORMED;
    } else if (ack->reject_reason == EFS_HELLO_REJECT_FULL) {
        fprintf(stderr, "Join refused by %s:%u (cluster %016llx) — the cluster has the maximum "
                "of %d members (%u now); this store (%016llx) at %s:%u cannot join it.\n",
                peer_host, peer_port, (unsigned long long)ack->cluster, EFS_MAX_NODES,
                ack->node_count, (unsigned long long)s->mstore.token, s->addr, s->port);
        rc = EFS_JOIN_REFUSED_FORMED;
    } else if ((ack->reject_reason == EFS_HELLO_PROMISE ||
                ack->reject_reason == EFS_HELLO_REJECT_NONE) &&
               ack->record_len && ack->record_len <= sizeof(ack->record) &&
               efs_membership_decode(ack->record, ack->record_len, &m) == EFS_OK &&
               m.cluster == ack->cluster) {
        const struct efs_member *self = efs_membership_find(&m, ack->assigned_id);
        if (!self || self->token != s->mstore.token || self->port != s->port ||
            strcmp(self->addr, s->addr)) {
            fprintf(stderr, "Join: %s:%u sent a record that does not name this store at "
                    "%s:%u; ignored\n", peer_host, peer_port, s->addr, s->port);
        } else {
            int promise = ack->reject_reason == EFS_HELLO_PROMISE;
            /* K33 test hook: the record arrived, nothing is persisted yet. */
            if (!promise)
                server_test_member_exit("before-learn");
            rc = promise ? member_store_promise(&s->mstore, &m, err, sizeof(err))
                         : member_store_learn(&s->mstore, &m, err, sizeof(err));
            /* K33 test hook: persisted, not acknowledged and not serving. */
            if (!promise && rc == EFS_OK)
                server_test_member_exit("after-learn");
            if (rc != EFS_OK) {
                fprintf(stderr, "Join: %s refused: %s\n", promise ? "promise" : "membership",
                        err);
                rc = rc == EFS_ERR_FENCED ? EFS_JOIN_REFUSED_FORMED : -1;
            } else {
                s->id = self->id;
                printf("Joined cluster %016llx as node %u at %s:%u (%s: %u members)\n",
                       (unsigned long long)m.cluster, self->id, s->addr, s->port,
                       promise ? "promised the formation proposal" : "committed member",
                       m.count);
                rc = EFS_JOIN_ADMITTED;
            }
        }
    } else {
        fprintf(stderr, "Join: %s:%u answered reason %u without a usable record\n", peer_host,
                peer_port, ack->reject_reason);
    }
    free(payload);
    return rc;
}

/* K33 test hook (tests/test_k33_growth.py, tests/test_k33_interrupt.py),
 * EFS_FAULTS builds only (tests/faults/efsd):
 * EFS_FAULT_MEMBER_EXIT=<point>[,<point>...] ends the process with status 86
 * at that durable handoff, so a test interrupts it deterministically. With
 * EFS_FAULT_MEMBER_EXIT_ONCE=<prefix> only the first process to create the
 * file <prefix>.<point> exits there (a cluster-wide one-shot per point: the
 * env can be given to every member, the one that reaches the point first
 * ends, the others and every restart continue). Points: after-members-commit,
 * before-learn, after-learn, learner-added, joint-started, after-g2-copy
 * (raft_host.c and the joiner path above), before-identity (store
 * initialization, boot.bin written and identity.bin not: member_store.c)
 * and before-hello (a joining identity persisted, no HELLO sent: efsd.c;
 * both tests/test_k33_join_retry.py), data-commit (a DATA_COMMIT received,
 * before its barrier: K35, tests/test_k35_fsync_degraded.py). */
void server_test_member_exit(const char *point)
{
#if EFS_FAULTS
    const char *e = getenv("EFS_FAULT_MEMBER_EXIT"), *once = getenv("EFS_FAULT_MEMBER_EXIT_ONCE");
    size_t n = strlen(point);
    int hit = 0;

    for (const char *q = e; q && n && !hit;) {
        hit = !strncmp(q, point, n) && (q[n] == ',' || q[n] == '\0');
        q = strchr(q, ',');
        if (q)
            q++;
    }
    if (!hit)
        return;
    if (once && *once) {
        char path[4096];
        snprintf(path, sizeof(path), "%s.%s", once, point);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0)
            return;
        close(fd);
    }
    fprintf(stderr, "membership: EFS_FAULT_MEMBER_EXIT at %s; exiting\n", point);
    fflush(stderr);
    fflush(stdout);
    efs_fault_exit(86);
#else
    (void)point;
#endif
}

void server_nodes_keep_members(struct efsd_server *s, const struct efs_membership *m)
{
    uint32_t kept = 0, old;
    int dirty = 0;
    pthread_mutex_lock(&s->lock);
    old = s->node_count;
    for (uint32_t i = 0; i < s->node_count && i < EFS_MAX_NODES; i++) {
        if (!efs_membership_find(m, s->nodes[i].id)) {
            fprintf(stderr, "membership: node %u (%s:%u) is not a committed member; dropped "
                    "from the node table\n", s->nodes[i].id, s->nodes[i].addr,
                    s->nodes[i].port);
            continue;
        }
        if (kept != i)
            s->nodes[kept] = s->nodes[i];
        kept++;
    }
    s->node_count = kept;
    dirty = kept != old;
    /* K33: a member learned from a revision enters the table at its
     * committed endpoint; its HELLO later fills storage and usage. */
    for (uint32_t i = 0; i < m->count && s->node_count < EFS_MAX_NODES; i++) {
        uint32_t j;
        for (j = 0; j < s->node_count && s->nodes[j].id != m->m[i].id; j++)
            ;
        if (j < s->node_count)
            continue;
        struct efs_node *n = &s->nodes[s->node_count++];
        memset(n, 0, sizeof(*n));
        n->id = m->m[i].id;
        snprintf(n->addr, sizeof(n->addr), "%s", m->m[i].addr);
        n->port = m->m[i].port;
        n->token = m->m[i].token;
        dirty = 1;
        fprintf(stderr, "membership: member %u (%s:%u) added to the node table\n", n->id,
                n->addr, n->port);
    }
    server_heartbeat_apply_locked(s);
    if (dirty)
        server_nodes_mark_dirty(s);
    pthread_mutex_unlock(&s->lock);
    server_nodes_flush_dirty(s);
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
    EFS_NODE_TABLE(nodes);
    if (!nodes)
        return;
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
 * and its per-cycle probe threads touch hb_conn[]; slot = node id - 1, one
 * probe per slot per cycle. */
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

/* A cached conn is reused only for the same host:port (a member may move). */
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

/* Heartbeat thread: ping peers, track fail streaks, and mark unresponsive
 * nodes down (excluded from placement) so a dead node does not keep
 * absorbing PUT/placement forever. No usage flush here.
 *
 * K41: every probe of a cycle runs in its own thread under a network
 * deadline of EFS_HB_PROBE_MS. Probes used to run one after another with
 * the 30 s connection timeouts: a committed member still in its join loop
 * (listening, not yet accepting) held each probe for 30 s, so the group-0
 * leader probed a stopped member once a minute and never marked it down
 * while the others did within 7 s; its packer placed a fragment there
 * (tests/test_k33_growth.py). The streak and the mark are kept per node id
 * here and written into every row with that id after each cycle, so a
 * table rebuilt in between (join adopt, membership revision, HELLO) cannot
 * reset or drop them: EFS_HB_DOWN_FAILS missed heartbeats mark a member
 * down on every node within EFS_HB_DOWN_FAILS cycles of at most
 * EFS_HEARTBEAT_MS + EFS_HB_PROBE_MS each. */
#define EFS_HB_DOWN_FAILS 3
#define EFS_HB_DOWN_MS    30000
#define EFS_HB_PROBE_MS   EFS_HEARTBEAT_MS
struct hb_peer {
    uint32_t streak;
    uint64_t down_until_ms; /* 0 = up; otherwise down (the deadline only spaces the log) */
};
static struct hb_peer hb_peer[EFS_MAX_NODES + 1]; /* by node id, under s->lock */

struct hb_probe {
    efs_node_id_t id;
    char addr[64];
    uint16_t port;
    int ok;
};

static void *hb_probe_run(void *arg)
{
    struct hb_probe *p = arg;
    efs_net_set_deadline_ms(hb_now_ms() + EFS_HB_PROBE_MS);
    p->ok = send_heartbeat(p->id - 1u, p->addr, p->port) == 0;
    efs_net_set_deadline_ms(0);
    return NULL;
}

/* Write the per-id liveness into the node table (s->lock held, which also
 * guards hb_peer[]). Every path that rebuilds or extends the table calls
 * it, so a row never carries another process's (or boot's) monotonic
 * deadline nor loses this node's mark. */
void server_heartbeat_apply_locked(struct efsd_server *s)
{
    for (uint32_t j = 0; j < s->node_count && j < EFS_MAX_NODES; j++) {
        efs_node_id_t id = s->nodes[j].id;
        if (!id || id > EFS_MAX_NODES || id == s->id)
            continue;
        s->nodes[j].hb_fail_streak = hb_peer[id].streak;
        s->nodes[j].down_until_ms = hb_peer[id].down_until_ms;
    }
}

/* The wait mutex is private to the heartbeat lifecycle, never s->lock.
 * Stop publishes running=0 then takes this mutex to wake the waiter, so it
 * cannot signal between the predicate check and the atomic wait/unlock.
 * Spurious wakes keep the same monotonic deadline and do not run probes. */
static int heartbeat_wait(struct efsd_server *s)
{
    struct timespec due;
    clock_gettime(CLOCK_MONOTONIC, &due);
    due.tv_sec += EFS_HEARTBEAT_MS / 1000;
    due.tv_nsec += (EFS_HEARTBEAT_MS % 1000) * 1000000L;
    if (due.tv_nsec >= 1000000000L) {
        due.tv_sec++;
        due.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->heartbeat_lock);
    int rc = 0;
    while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE) && !rc)
        rc = pthread_cond_timedwait(&s->heartbeat_wake, &s->heartbeat_lock, &due);
    int run = __atomic_load_n(&s->running, __ATOMIC_ACQUIRE) && rc == ETIMEDOUT;
    pthread_mutex_unlock(&s->heartbeat_lock);
    if (rc && rc != ETIMEDOUT)
        fprintf(stderr, "heartbeat: wait failed: %s\n", strerror(rc));
    return run;
}

static void *heartbeat_thread(void *arg)
{
    struct efsd_server *s = arg;
    /* Heap: efs_node is ~4 KiB (storage_path); EFS_MAX_NODES of them on the
     * stack plus usage_save path buffers blew the ASAN-instrumented stack. */
    struct efs_node *nodes = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
    struct hb_probe *probe = calloc(EFS_MAX_NODES, sizeof(*probe));
    pthread_t tid[EFS_MAX_NODES];
    int started[EFS_MAX_NODES];
    if (!nodes || !probe) {
        free(nodes);
        free(probe);
        return NULL;
    }

    uint64_t gc_log_ms = 0, trim_ms = hb_now_ms();
    while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
        if (!heartbeat_wait(s))
            break;

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
        if (now - trim_ms >= EFSD_MALLOC_TRIM_MS) {
            efsd_malloc_trim();
            trim_ms = now;
        }
        /* One probe per member id (the table is deduplicated; a repeated
         * id is probed once). */
        uint32_t np = 0;
        for (uint32_t i = 0; i < node_count; i++) {
            efs_node_id_t id = nodes[i].id;
            int dup = !id || id > EFS_MAX_NODES || id == self;
            for (uint32_t k = 0; k < np && !dup; k++)
                dup = probe[k].id == id;
            if (dup)
                continue;
            probe[np].id = id;
            snprintf(probe[np].addr, sizeof(probe[np].addr), "%s", nodes[i].addr);
            probe[np].port = nodes[i].port;
            probe[np].ok = 0;
            np++;
        }
        for (uint32_t k = 0; k < np; k++)
            started[k] = efsd_pthread_create(&tid[k], hb_probe_run, &probe[k]) == 0;
        for (uint32_t k = 0; k < np; k++) {
            if (started[k])
                pthread_join(tid[k], NULL);
            else
                hb_probe_run(&probe[k]);
        }

        for (uint32_t k = 0; k < np; k++)
            if (probe[k].ok)
                server_peer_note_up(probe[k].addr, probe[k].port);
        now = hb_now_ms();
        pthread_mutex_lock(&s->lock);
        for (uint32_t k = 0; k < np; k++) {
            struct hb_peer *hp = &hb_peer[probe[k].id];
            if (probe[k].ok) {
                hp->streak = 0;
                hp->down_until_ms = 0;
                continue;
            }
            if (hp->streak < 1000)
                hp->streak++;
            if (hp->streak >= EFS_HB_DOWN_FAILS && hp->down_until_ms <= now) {
                hp->down_until_ms = now + EFS_HB_DOWN_MS;
                fprintf(stderr,
                        "heartbeat: node %u (%s:%u) unresponsive — "
                        "marked down for %ds\n",
                        probe[k].id, probe[k].addr, probe[k].port,
                        EFS_HB_DOWN_MS / 1000);
            }
        }
        server_heartbeat_apply_locked(s);
        pthread_mutex_unlock(&s->lock);
    }
    free(nodes);
    free(probe);
    return NULL;
}

void server_start_heartbeat(struct efsd_server *s)
{
    /* Teardown joins before freeing the Raft host or closing peer pools:
     * a heartbeat already inside server_gc_status still owns that host. */
    pthread_condattr_t attr;
    int rc = pthread_mutex_init(&s->heartbeat_lock, NULL);
    if (rc) goto failed;
    rc = pthread_condattr_init(&attr);
    if (rc) goto mutex_failed;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc) rc = pthread_cond_init(&s->heartbeat_wake, &attr);
    pthread_condattr_destroy(&attr);
    if (rc) goto mutex_failed;
    rc = efsd_pthread_create(&s->heartbeat_tid, heartbeat_thread, s);
    if (rc) {
        pthread_cond_destroy(&s->heartbeat_wake);
        goto mutex_failed;
    }
    s->heartbeat_started = 1;
    fprintf(stderr, "heartbeat: enabled (%d ms interval)\n", EFS_HEARTBEAT_MS);
    return;
mutex_failed:
    pthread_mutex_destroy(&s->heartbeat_lock);
failed:
    fprintf(stderr, "heartbeat: start failed: %s\n", strerror(rc));
}

void server_stop_heartbeat(struct efsd_server *s)
{
    __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
    if (s->heartbeat_started) {
        pthread_mutex_lock(&s->heartbeat_lock);
        pthread_cond_broadcast(&s->heartbeat_wake);
        pthread_mutex_unlock(&s->heartbeat_lock);
        pthread_join(s->heartbeat_tid, NULL);
        s->heartbeat_started = 0;
        pthread_cond_destroy(&s->heartbeat_wake);
        pthread_mutex_destroy(&s->heartbeat_lock);
    }
}

/* Rejoin thread: periodically retry joining until this server is part of a
 * cluster with at least one other node. Uses the explicit --join target when
 * set, otherwise the persisted peer list. */
static void *rejoin_thread(void *arg)
{
    struct efsd_server *s = arg;
    int first = 1;
    while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
        if (!first || !s->rejoin_announce)
            sleep(2);
        first = 0;

        pthread_mutex_lock(&s->lock);
        uint32_t node_count = s->node_count;
        pthread_mutex_unlock(&s->lock);

        /* Already in a cluster with at least one peer and a member: done.
         * K33: an unformed node (a pending joiner, or one formation left
         * out) keeps saying HELLO until its store learned a revision; a
         * restarted member until one persisted peer answered (announce). */
        if (node_count > 1 && server_raft_host_formed() && !s->rejoin_announce)
            break;

        int rc;
        if (s->rejoin_addr[0]) {
            rc = server_join_cluster(s, s->rejoin_addr, s->rejoin_port);
            if (rc == EFS_JOIN_REFUSED_FORMED) {
                fprintf(stderr, "Stopping: this node cannot join the formed cluster\n");
                __atomic_store_n(&s->join_refused, 1, __ATOMIC_RELEASE);
                __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
                break;
            }
            if (rc == 0 && !server_raft_host_formed()) {
                /* Before formation, or a member that missed the notice:
                 * the HELLO refreshed the table; keep going. */
            } else if (rc == 0) {
                printf("Rejoined cluster via %s:%u\n", s->rejoin_addr, s->rejoin_port);
                pthread_mutex_lock(&s->lock);
                server_nodes_mark_dirty(s);
                pthread_mutex_unlock(&s->lock);
                server_nodes_flush_dirty(s);
            }
        } else {
            rc = server_rejoin_cluster(s);
            if (rc == 0) {
                s->rejoin_announce = 0;
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
