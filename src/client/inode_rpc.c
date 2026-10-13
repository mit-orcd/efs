#include "client_internal.h"
#include "efs/opid.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/publication.h"
#include "efs/put_ticket.h"
#include "efs/session.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

/* EFS_RPC_PROF: split inode-RPC wall time into conn checkout vs send vs
 * recv vs BUSY backoff. Recv is the server+wire wait (the parked H3
 * question). Checkout is pool serialization (16 conns/node). Off unless
 * the env is set — empty still counts as on, same as EFS_LOCK_PROF. */
static int efs_rpc_prof_on = -1;
static unsigned long long efs_rpc_calls;
static unsigned long long efs_rpc_checkout_us;
static unsigned long long efs_rpc_send_us;
static unsigned long long efs_rpc_recv_us;
static unsigned long long efs_rpc_busy_us;
static unsigned long long efs_rpc_busy_n;
static unsigned long long efs_rpc_checkout_wait_n;
static unsigned long long efs_rpc_n_op[256];
static unsigned long long efs_rpc_recv_us_op[256];
static unsigned long long efs_rpc_busy_n_op[256];
static unsigned long long efs_rpc_last_dump_us;

static unsigned long long rpc_prof_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull +
           (unsigned long long)ts.tv_nsec / 1000ull;
}

/* An inode RPC that returned OK only after the retry loop had already
 * spent a second. attempts is the try that succeeded (1 = first try, so
 * the second was a single slow round, not the BUSY ladder). The holder
 * go-timeouts had no line at all. */
static void rpc_note_slow_ok(uint8_t type, int attempt, int saw_busy,
                             int status, unsigned long long t0)
{
    unsigned long long us;

    if (status != EFS_INODE_RPC_OK || t0 == 0)
        return;
    us = rpc_prof_now_us() - t0;
    if (us < 1000000ull)
        return;
    fprintf(stderr, "inode-rpc: slow-ok type=%u attempts=%d saw_busy=%d "
            "status=%d us=%llu\n",
            type, attempt + 1, saw_busy, status, us);
}

static int rpc_prof_enabled(void)
{
    if (efs_rpc_prof_on < 0)
        efs_rpc_prof_on = getenv("EFS_RPC_PROF") != NULL;
    return efs_rpc_prof_on;
}

static void rpc_prof_add(uint8_t type, unsigned long long checkout_us,
                         unsigned long long send_us, unsigned long long recv_us,
                         unsigned long long busy_us)
{
    if (!rpc_prof_enabled())
        return;
    __atomic_add_fetch(&efs_rpc_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_checkout_us, checkout_us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_send_us, send_us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_recv_us, recv_us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_busy_us, busy_us, __ATOMIC_RELAXED);
    if (busy_us) {
        __atomic_add_fetch(&efs_rpc_busy_n, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&efs_rpc_busy_n_op[type], 1, __ATOMIC_RELAXED);
    }
    if (checkout_us >= 100ull)
        __atomic_add_fetch(&efs_rpc_checkout_wait_n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_n_op[type], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_recv_us_op[type], recv_us, __ATOMIC_RELAXED);
    unsigned long long now = rpc_prof_now_us();
    unsigned long long last = __atomic_load_n(&efs_rpc_last_dump_us,
                                            __ATOMIC_RELAXED);
    if (last && now - last < 2000000ull)
        return;
    if (!__atomic_compare_exchange_n(&efs_rpc_last_dump_us, &last, now, 0,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    fprintf(stderr,
            "RPC-PROF calls=%llu checkout_us=%llu send_us=%llu recv_us=%llu "
            "busy_us=%llu busy_n=%llu checkout_wait_n=%llu "
            "lookup=%llu/%lluus getattr=%llu/%lluus create=%llu/%lluus "
            "setattr=%llu/%lluus readdir=%llu/%lluus unlink=%llu/%lluus "
            "append=%llu/%lluus report=%llu/%lluus getchunks=%llu/%lluus "
            "lookup_path=%llu/%lluus\n",
            __atomic_load_n(&efs_rpc_calls, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_checkout_us, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_send_us, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_busy_us, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_busy_n, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_checkout_wait_n, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_LOOKUP], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_LOOKUP],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_GETATTR], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_GETATTR],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_CREATE], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_CREATE],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_SETATTR], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_SETATTR],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_READDIR], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_READDIR],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_UNLINK], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_UNLINK],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_APPEND], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_APPEND],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_REPORT_CHUNKS], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_REPORT_CHUNKS],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_GETCHUNKS],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_GETCHUNKS],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_LOOKUP_PATH],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_LOOKUP_PATH],
                            __ATOMIC_RELAXED));
}

static int rpc_status_to_efs(uint8_t st)
{
    switch (st) {
    case EFS_INODE_RPC_OK:          return EFS_OK;
    case EFS_INODE_RPC_NOT_FOUND:   return EFS_ERR_NOT_FOUND;
    case EFS_INODE_RPC_EXIST:       return EFS_ERR_EXIST;
    case EFS_INODE_RPC_QUOTA:       return EFS_ERR_QUOTA;
    case EFS_INODE_RPC_BUSY:
    case EFS_INODE_RPC_ADMISSION_BUSY: return EFS_ERR_BUSY;
    case EFS_INODE_RPC_INVAL:       return EFS_ERR_INVAL;
    case EFS_INODE_RPC_NOT_PRIMARY: return EFS_ERR_NOT_PRIMARY;
    case EFS_INODE_RPC_NOT_EMPTY:   return EFS_ERR_NOT_EMPTY;
    case EFS_INODE_RPC_STALE:       return EFS_ERR_STALE;
    case EFS_INODE_RPC_NODATA:      return EFS_ERR_NODATA;
    case EFS_INODE_RPC_OVERFLOW:    return EFS_ERR_OVERFLOW;
    case EFS_INODE_RPC_SYMLINK:
    case EFS_INODE_RPC_DEEP:        return EFS_ERR_PROTO;
    default:                        return EFS_ERR_IO;
    }
}

/* Namespace mutations whose txn may PREPARE STALE against a moved dentry,
 * child row or emptiness witness. None of them has committed anything when
 * the server says STALE, and each re-evaluates from scratch, so the client
 * retries them like BUSY. REPORT_CHUNKS / APPEND own their STALE. */
static int stale_retryable(uint8_t type)
{
    return type == EFS_MSG_INODE_CREATE || type == EFS_MSG_INODE_UNLINK ||
           type == EFS_MSG_INODE_LINK || type == EFS_MSG_INODE_RENAME_AT;
}

/* I16 op-id for the directory mutations above (§7.9). One identity per
 * mount (random uuid, epoch 1), one seq space, and an in-flight table:
 * the contiguous ack sent with every request is (lowest in-flight seq)
 * - 1, so a server window advances past seqs it never served. A slot is
 * held from before the first send to after the final return — every
 * BUSY / STALE / NOT_PRIMARY retry inside rpc_send_recv_* preserves the
 * identity and operation payload. Retries may advance only their contiguous
 * ACK as older operations finish. Table full = the op
 * receives BUSY before any RPC; no mutation loses its identity. */
static pthread_mutex_t opid_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t opid_cv = PTHREAD_COND_INITIALIZER;

static void opid_seed_locked(void)
{
    int fd, got = 0;

    if (g_client.opid_next)
        return;
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, g_client.opid_uuid, EFS_OPID_UUID_LEN);
        close(fd);
        got = n == (ssize_t)EFS_OPID_UUID_LEN;
    }
    if (!got) {
        uint64_t a = (uint64_t)getpid() * 0x9E3779B97F4A7C15ull ^
                     (uint64_t)time(NULL);
        uint64_t b = (uint64_t)(uintptr_t)&g_client ^ 0xD1B54A32D192ED03ull;
        memcpy(g_client.opid_uuid, &a, 8);
        memcpy(g_client.opid_uuid + 8, &b, 8);
    }
    g_client.opid_uuid[0] |= 1; /* never the all-zero "no identity" */
    g_client.opid_epoch = 1;
    g_client.opid_next = 1;
    memset(g_client.opid_inflight, 0, sizeof(g_client.opid_inflight));
}

int efs_client_publication_id(uint64_t sequence, struct efs_opid *out)
{
    if(!sequence || !out)return EFS_ERR_INVAL;
    memset(out,0,sizeof(*out));
    pthread_mutex_lock(&opid_mu);opid_seed_locked();
    memcpy(out->client_uuid,g_client.opid_uuid,EFS_OPID_UUID_LEN);
    out->session_epoch=g_client.opid_epoch;out->seq=sequence;
    pthread_mutex_unlock(&opid_mu);return EFS_OK;
}

int efs_client_put_id(struct efs_opid *out)
{
    if (!out) return EFS_ERR_INVAL;
    memset(out,0,sizeof(*out));
    pthread_mutex_lock(&opid_mu);opid_seed_locked();
    if (!g_client.put_next) g_client.put_next=1;
    if (g_client.put_next==UINT64_MAX) {
        pthread_mutex_unlock(&opid_mu);return EFS_ERR_BUSY;
    }
    memcpy(out->client_uuid,g_client.opid_uuid,EFS_OPID_UUID_LEN);
    out->session_epoch=g_client.opid_epoch;out->seq=g_client.put_next++;
    pthread_mutex_unlock(&opid_mu);return EFS_OK;
}

/* Fill q and take a slot (returned; -1 = none, q left invalid). */
static int opid_begin(struct efs_opid_req *q)
{
    memset(q,0,sizeof(*q));
    /* A later known reply still occupies the server cache until the oldest
     * unresolved identity finishes. Reserve space for that oldest identity:
     * never issue more sequence positions than the smallest receipt cache.
     * Otherwise later replies fill it and the missing oldest cannot commit. */
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    uint64_t deadline = efs_client_rpc_deadline_ms();
    if (!deadline)
        deadline = (uint64_t)started.tv_sec * 1000 + started.tv_nsec / 1000000 + 1000;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (now_ms >= deadline || efs_client_rpc_past_deadline()) break;
        int slot=-1;uint64_t low=0;
        pthread_mutex_lock(&opid_mu);
        opid_seed_locked();
        for(unsigned i=0;i<EFS_OPID_INFLIGHT;i++) {
            uint64_t seq=g_client.opid_inflight[i];
            if(!seq){if(slot<0)slot=(int)i;}
            else if(!low || seq<low)low=seq;
        }
        int exhausted=g_client.opid_next==UINT64_MAX;
        if(slot>=0 && !exhausted &&
           (!low || g_client.opid_next-low<EFS_OPID_REPLY_CACHE)) {
            uint64_t seq=g_client.opid_next++;
            g_client.opid_inflight[slot]=seq;
            memcpy(q->id.client_uuid,g_client.opid_uuid,EFS_OPID_UUID_LEN);
            q->id.session_epoch=g_client.opid_epoch;q->id.seq=seq;
            q->ack=(low?low:seq)-1;
            pthread_mutex_unlock(&opid_mu);return slot;
        }
        if (exhausted) {
            pthread_mutex_unlock(&opid_mu);
            return -2; /* sequence exhaustion is permanent */
        }
        /* No identity has been issued. Wait for an owner to finish within
         * the caller's existing budget, rather than exhausting an unrelated
         * one-second poll count while a valid older operation still runs.
         * Periodic wakeups recheck cancellation/deadline without holding
         * the mutex needed by the finishing owner. */
        uint64_t wait_ms = deadline - now_ms;
        if (wait_ms > 50) wait_ms = 50;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)wait_ms * 1000000;
        until.tv_sec += until.tv_nsec / 1000000000;
        until.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&opid_cv, &opid_mu, &until);
        pthread_mutex_unlock(&opid_mu);
    }
    uint64_t oldest=0,next;unsigned live=0;
    pthread_mutex_lock(&opid_mu);
    next=g_client.opid_next;
    for(unsigned i=0;i<EFS_OPID_INFLIGHT;i++) {
        uint64_t seq=g_client.opid_inflight[i];
        if(seq){live++;if(!oldest || seq<oldest)oldest=seq;}
    }
    pthread_mutex_unlock(&opid_mu);
    fprintf(stderr,"opid-admission: BUSY oldest=%llu next=%llu live=%u window=%u\n",
        (unsigned long long)oldest,(unsigned long long)next,live,EFS_OPID_REPLY_CACHE);
    return -1;
}

static void opid_end(int slot)
{
    if (slot < 0)
        return;
    pthread_mutex_lock(&opid_mu);
    g_client.opid_inflight[slot] = 0;
    pthread_cond_broadcast(&opid_cv);
    pthread_mutex_unlock(&opid_mu);
}

/* A reservation identity lives across the entire FUSE BUSY retry loop,
 * not just one transport attempt. Share namespace's contiguous-ACK tracker;
 * append's distinct KV kind keeps its result namespace separate. The full
 * mount UUID is shared with lease and lock ownership. */
int efs_client_append_begin(struct efs_opid_req *q)
{
    return opid_begin(q);
}

void efs_client_append_end(int slot)
{
    opid_end(slot);
}

/* Append the wire suffix after `slen` bytes of struct; new length. */
static uint32_t opid_suffix(uint8_t *buf, uint32_t slen, const struct efs_opid_req *q)
{
    if (!efs_opid_req_valid(q))
        return slen;
    efs_opid_req_pack(q, buf + slen);
    return slen + EFS_OPID_WIRE_LEN;
}

/* The identity is immutable, but its contiguous ACK may advance while a
 * retry waits behind older operations. Never ACK this operation itself. */
static void opid_refresh_ack(struct efs_opid_req *q)
{
    if (!efs_opid_req_valid(q)) return;
    uint64_t low=0;int owned=0;
    pthread_mutex_lock(&opid_mu);
    for (unsigned i=0;i<EFS_OPID_INFLIGHT;i++) {
        uint64_t seq=g_client.opid_inflight[i];
        if (seq==q->id.seq) owned=1;
        if (seq && (!low || seq<low)) low=seq;
    }
    if (owned && low && low-1>q->ack && low<=q->id.seq &&
        q->id.session_epoch==g_client.opid_epoch &&
        !memcmp(q->id.client_uuid,g_client.opid_uuid,EFS_OPID_UUID_LEN))
        q->ack=low-1;
    pthread_mutex_unlock(&opid_mu);
}
static void opid_retry_ack(void *packet, uint32_t length)
{
    struct efs_opid_req q;
    uint8_t *suffix=(uint8_t *)packet+length-EFS_OPID_WIRE_LEN;
    efs_opid_req_unpack(&q,suffix);
    opid_refresh_ack(&q);
    efs_opid_req_pack(&q,suffix);
}

/* Phase 2b: the metadata primary is the lowest-id live node. Mutations must
 * reach it (the meta-flush thread is primary-only). */
static struct efs_conn *rpc_primary_conn(efs_node_id_t *nid_out)
{
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = 0;
    for (uint32_t i = 0; i < g_client.node_count && nlive < EFS_MAX_NODES; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        if (efs_client_node_is_down(id))
            continue;
        live[nlive++] = id;
    }
    efs_node_id_t best = efs_shard_owner_of(0, 1, live, nlive);
    if (best == 0)
        return NULL;
    struct efs_conn *conn = efs_client_conn_get(best);
    if (conn)
        *nid_out = best;
    return conn;
}

/* Route to the owner of an explicit shard id (not an inode). Do not
 * invent an ino to fake this — crafted inos collide with ROOT and
 * mis-route GETCHUNKS/REPORT after extent sharding. */

/* The metadata engine is the Raft+KV host, never the in-memory table.
 * Shards map to Raft groups by the
 * compiled-in rule (odd shard -> group 0, even -> group 2;
 * include/efs/raft.h), and each group's voters are a fixed node set
 * (group 0 = nodes 1,2,3; group 2 = nodes 2,3,4 for a 4-node cluster).
 * Any voter can answer or bounce the RPC to the leader, and
 * rpc_send_recv_shard follows the primary_id hint, so the client only
 * needs to reach *a* live voter of the right group. */
static uint32_t raft_group_voters(uint8_t group, int n)
{
    if (n <= 3)
        return (1u << n) - 1u;
    if (group == EFS_RAFT_GROUP_SHARD)
        return 0x7u; /* nodes 1,2,3 = raft ids 0,1,2 */
    return 0xeu;     /* nodes 2,3,4 = raft ids 1,2,3 */
}

/* Leader of each metadata group as the last OK reply named it
 * (set_inode_rc puts the ReadIndex group's leader in primary_id on OK
 * too). A follower-served read is a ReadIndex round trip to the leader
 * plus a catch-up wait before the KV get; sending it to the leader
 * removes that hop (Spark du Oct 2: 2.5 ms per stat on the follower
 * host vs 1.4 on the leader host). A wrong note costs the old path: the
 * leader forwards or answers NOT_PRIMARY with the hint and the reply
 * corrects the note. 0 = unknown; indexed by group >> 1. */
static efs_node_id_t g_group_leader[2];

static void rpc_note_leader(uint8_t group, efs_node_id_t nid)
{
    unsigned gi = group >> 1;

    if (gi >= 2 || nid == 0 || nid > EFS_RAFT_MAX_PEERS)
        return;
    __atomic_store_n(&g_group_leader[gi], nid, __ATOMIC_RELAXED);
}

static efs_node_id_t rpc_leader_of(uint8_t group)
{
    unsigned gi = group >> 1;

    if (gi >= 2)
        return 0;
    return __atomic_load_n(&g_group_leader[gi], __ATOMIC_RELAXED);
}

/* The last OK LOOKUP/GETATTR reply's leader note, keyed by the group of
 * the inode the reply describes (a LOOKUP's hint is the child's group,
 * host_read_inode_lanes sets it last). */
static void rpc_learn_leader(const struct efs_msg_inode_reply *r)
{
    if (!r || r->status != EFS_INODE_RPC_OK || r->primary_id == 0 ||
        r->inode.ino == 0)
        return;
    rpc_note_leader(efs_raft_shard_group(efs_kv_inode_shard(r->inode.ino)),
                    r->primary_id);
}

/* First live voter of the shard's group, not `skip`; the host bounces to
 * the leader and the retry loop follows primary_id, so any voter is a
 * fine entry point. The group's noted leader goes first when it is a live
 * voter (see g_group_leader). `skip` is the voter that just answered NOT_PRIMARY
 * with no hint: this pick is deterministic, so without it the same node
 * was asked 16 times over 10.3 s and the op failed EBUSY (Sep 29, fstor007
 * ecopy: 810 such lines, SETATTR/CREATE/RENAME_AT, both terms stable).
 * When every other voter is down the skipped one is still the answer. */
static struct efs_conn *raft_voter_conn_skip(uint32_t shard,
                                             efs_node_id_t *nid_out,
                                             efs_node_id_t skip)
{
    uint8_t group = efs_raft_shard_group(shard);
    int n = (int)g_client.node_count;
    uint32_t voters = raft_group_voters(group, n);
    efs_node_id_t lead = rpc_leader_of(group);
    int pass;

    if (lead && lead != skip && lead <= (efs_node_id_t)n &&
        (voters & (1u << (lead - 1))) && !efs_client_node_is_down(lead)) {
        struct efs_conn *conn = efs_client_conn_get(lead);
        if (conn) {
            *nid_out = lead;
            return conn;
        }
    }
    for (pass = 0; pass < 2; pass++) {
        for (int rid = 0; rid < n && rid < EFS_RAFT_MAX_PEERS; rid++) {
            if (!(voters & (1u << rid)))
                continue;
            efs_node_id_t id = (efs_node_id_t)(rid + 1);
            if (pass == 0 && skip && id == skip)
                continue;
            if (efs_client_node_is_down(id))
                continue;
            struct efs_conn *conn = efs_client_conn_get(id);
            if (conn) {
                *nid_out = id;
                return conn;
            }
        }
        if (!skip)
            break;
    }
    return rpc_primary_conn(nid_out);
}

static struct efs_conn *raft_voter_conn(uint32_t shard,
                                        efs_node_id_t *nid_out)
{
    return raft_voter_conn_skip(shard, nid_out, 0);
}

/* A voter of EVERY metadata group. Reports carry recs for many inos whose
 * inode groups and lane groups differ, and only a dual-host can apply the
 * whole batch locally (the server forwards otherwise — this just saves the
 * hop). Whenever both groups are writable some dual-host is up: with the
 * compiled-in 4-node mapping (voters {1,2,3} and {2,3,4}) two quorums
 * always share a live node. Falls back to any group-0 voter when no
 * dual-host is reachable — the server side forwards from there. */
static struct efs_conn *raft_dual_voter_conn(efs_node_id_t *nid_out,
                                            efs_node_id_t skip)
{
    int n = (int)g_client.node_count;
    uint32_t dual = raft_group_voters(EFS_RAFT_GROUP_SHARD, n) &
                    raft_group_voters(EFS_RAFT_GROUP_SHARD2, n);

    /* Try the excluded peer only after the other dual hosts. Checking out
     * its socket first can consume the whole RPC deadline on a failed peer
     * or a full pool before the healthy alternative is even attempted. */
    for (unsigned pass = 0; pass < 2; pass++) {
        for (int rid = 0; rid < n && rid < EFS_RAFT_MAX_PEERS; rid++) {
            efs_node_id_t id = (efs_node_id_t)(rid + 1);
            if (!(dual & (1u << rid)) ||
                (pass == 0 ? id == skip : !skip || id != skip) ||
                efs_client_node_is_down(id))
                continue;
            struct efs_conn *conn = efs_client_conn_get(id);
            if (conn) {
                *nid_out = id;
                return conn;
            }
        }
    }
    return raft_voter_conn(EFS_ROOT_INO, nid_out);
}

static struct efs_conn *rpc_owner_conn_shard(uint32_t shard,
                                             efs_node_id_t *nid_out)
{
    return raft_voter_conn(shard, nid_out);
}

static int rpc_send_recv_dual(uint8_t type, const void *req, uint32_t req_len,
                              uint8_t expect, void *reply, uint32_t reply_len,
                              int recv_ms);

/* Send to the owner of `shard`. Retry NOT_PRIMARY. */
static int rpc_writer_retry_pause(unsigned attempt);

#if EFS_FAULTS
/* Test-only: consume a committed CREATE reply, then hide a replay as BUSY.
 * Retained FUSE CREATE uses separate framing and may switch submit/probe or
 * advance its ACK. Its creation arguments, identity and durable result must
 * stay identical; an unresolved probe is never a committed reply. */
static int rpc_fault_create_reply(uint8_t type, const void *req, uint32_t len,
                                  uint8_t rtype, void *payload, uint32_t plen)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static unsigned stage;
    static struct efs_msg_inode_create original;
    static struct efs_opid identity;
    static efs_ino_t ino;
    static uint64_t generation;
    static uint32_t shard;
    static int original_retained;
    const char *marker = getenv("EFS_FAULT_CREATE_RETRY_FILE");
    if (!marker || access(marker,F_OK)) return 0;
    const struct efs_msg_inode_create *create;
    struct efs_msg_inode_reply *reply;
    struct efs_msg_create_retained_reply *retained_reply = NULL;
    struct efs_opid_req q;
    if (type == EFS_MSG_CREATE_RETAINED &&
        len == sizeof(struct efs_msg_create_retained) &&
        rtype == EFS_MSG_CREATE_RETAINED_REPLY &&
        plen == sizeof(struct efs_msg_create_retained_reply)) {
        const struct efs_msg_create_retained *r = req;
        retained_reply = payload;
        if (!retained_reply->known || !retained_reply->routed ||
            !retained_reply->generation) return 0;
        create = &r->create; q = r->q; reply = &retained_reply->rpc;
    } else if (type == EFS_MSG_INODE_CREATE &&
               len == sizeof(*create)+EFS_OPID_WIRE_LEN &&
               rtype == EFS_MSG_INODE_CREATE_REPLY &&
               plen == sizeof(*reply)) {
        create = req; reply = payload;
        efs_opid_req_unpack(&q,(const uint8_t *)req+sizeof(*create));
    } else return 0;
    if (strcmp(create->name,"w58-live-retry") || reply->status != EFS_INODE_RPC_OK)
        return 0;
    pthread_mutex_lock(&mu);
    if (stage >= 3) { pthread_mutex_unlock(&mu); return 0; }
    if (!stage) {
        original = *create; identity = q.id; ino = reply->inode.ino;
        original_retained = retained_reply != NULL;
        if (retained_reply) {
            generation = retained_reply->generation; shard = retained_reply->shard;
        }
    }
    if (memcmp(&original,create,sizeof(original)) ||
        identity.seq != q.id.seq || identity.session_epoch != q.id.session_epoch ||
        memcmp(identity.client_uuid,q.id.client_uuid,EFS_OPID_UUID_LEN) ||
        ino != reply->inode.ino || original_retained != (retained_reply != NULL) ||
        (retained_reply && (generation != retained_reply->generation ||
                            shard != retained_reply->shard))) abort();
    fprintf(stderr,"test-create-retry: stage=%u seq=%llu ino=%llu identical=1\n",
            stage,(unsigned long long)q.id.seq,(unsigned long long)ino);
    unsigned current=stage++;
    if (current == 1) {
        reply->status=EFS_INODE_RPC_BUSY;
        if (retained_reply) {
            retained_reply->known=0; retained_reply->generation=0;
            memset(&reply->inode,0,sizeof(reply->inode));
        }
    }
    pthread_mutex_unlock(&mu);
    return current == 0 ? -1 : 0;
}
#endif

/* A transport failure invalidates this attempt's hint. Preserve the
 * request identity while retrying another voter, and never erase a newer
 * leader learned concurrently by another worker. Pool admission failure
 * is not evidence that a peer is down. */
static void rpc_retry_peer(uint32_t shard, efs_node_id_t nid,
                            efs_node_id_t *target, efs_node_id_t *skip, int failed)
{
    unsigned group = efs_raft_shard_group(shard) >> 1;
    *target = 0;
    *skip = nid;
    if (!nid) return;
    if (failed) efs_client_node_note_fail(nid);
    if (group < 2) {
        efs_node_id_t expected = nid;
        __atomic_compare_exchange_n(&g_group_leader[group], &expected, 0,
                                    0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
}

static int rpc_send_recv_shard(uint32_t shard, uint8_t type, const void *req,
                               uint32_t req_len, uint8_t expect, void *reply,
                               uint32_t reply_len, uint32_t copy_cap,
                               uint32_t *copied)
{
    efs_node_id_t target = 0; /* 0 = compute the owner from our view */
    efs_node_id_t skip = 0;   /* answered NOT_PRIMARY with no hint */
    int prof = rpc_prof_enabled();
    int saw_busy = 0;
    unsigned long long t_start = rpc_prof_now_us();
    union { struct efs_msg_inode_create create; struct efs_msg_inode_unlink unlink;
            struct efs_msg_inode_link link; struct efs_msg_inode_rename_at rename; } mutation;
    uint8_t create_retry[sizeof(mutation)+EFS_OPID_WIRE_LEN];
    uint32_t flock_op = 0;
    if (type == EFS_MSG_INODE_FLOCK && req &&
        req_len >= sizeof(struct efs_msg_inode_flock))
        memcpy(&flock_op, (const uint8_t *)req +
               offsetof(struct efs_msg_inode_flock, op), sizeof(flock_op));
    int identified_create=(type==EFS_MSG_INODE_CREATE && req_len==sizeof(mutation.create)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_UNLINK && req_len==sizeof(mutation.unlink)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_LINK && req_len==sizeof(mutation.link)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_RENAME_AT && req_len==sizeof(mutation.rename)+EFS_OPID_WIRE_LEN);
    if (identified_create) { memcpy(create_retry,req,req_len);req=create_retry; }
    for (int attempt = 0; attempt < 16; attempt++) {
        if (identified_create) opid_retry_ack(create_retry,req_len);
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = 0;
        struct efs_conn *conn;
        unsigned long long t0 = prof ? rpc_prof_now_us() : 0;
        if (target != 0) {
            conn = efs_client_conn_get(target);
            nid = target;
        } else {
            conn = raft_voter_conn_skip(shard, &nid, skip);
        }
        unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
        if (!conn || efs_conn_send_msg(conn, type, req, req_len) != 0) {
            if (conn)
                efs_client_conn_drop(nid, conn);
            rpc_retry_peer(shard, nid, &target, &skip, conn != NULL);
            /* One dropped conn is not a failed create. Same 16-attempt
             * budget as BUSY; a hard NET only after it is used up
             * (dir_many_files EIO on a single recv failure). */
            if (attempt + 1 < 16) {
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=send\n",
                        type, attempt);
                if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                    return EFS_ERR_BUSY;
                continue;
            }
            return EFS_ERR_NET;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        uint8_t rtype = 0;
        void *payload = NULL;
        uint32_t plen = 0;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
#if EFS_FAULTS
        if (!rc) rc=rpc_fault_create_reply(type,req,req_len,rtype,payload,plen);
#endif
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            rpc_retry_peer(shard, nid, &target, &skip, 1);
            if (attempt + 1 < 16) {
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=recv rc=%d\n",
                        type, attempt, rc);
                if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                    return EFS_ERR_BUSY;
                continue;
            }
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        efs_client_node_note_ok(nid);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        if (rtype != expect || plen < reply_len) {
            fprintf(stderr, "inode-rpc: proto type=%u rtype=%u plen=%u "
                    "reply_len=%u nid=%u\n",
                    type, rtype, plen, reply_len, nid);
            free(payload);
            return EFS_ERR_PROTO;
        }
        {
            uint32_t cap = copy_cap ? copy_cap : reply_len;
            uint32_t ncopy;

            if (cap < reply_len) {
                free(payload);
                return EFS_ERR_PROTO;
            }
            ncopy = plen < cap ? plen : cap;
            memcpy(reply, payload, ncopy);
            if (copied)
                *copied = ncopy;
        }
        free(payload);
        struct efs_msg_inode_reply *r = reply;
        if (r->status == EFS_INODE_RPC_BUSY ||
            r->status == EFS_INODE_RPC_ADMISSION_BUSY) {
            saw_busy = 1;
            /* INODE_APPEND uses BUSY as the unflushed-reservation barrier.
             * The caller must flush + report before retrying; spinning here
             * holds g_append_mu for ~10s and deadlocks concurrent O_APPEND. */
            /* An unlock has no reservation/conflict barrier for its caller
             * to resolve. Retry the identical owner/session/range after
             * transient authority/proposal BUSY, with the existing budget.
             * Grants retain their immediate conflict response. */
            if (r->status == EFS_INODE_RPC_BUSY &&
                (type == EFS_MSG_INODE_APPEND ||
                (type == EFS_MSG_INODE_FLOCK && !(flock_op & EFS_FLOCK_UN)) ||
                type == EFS_MSG_CREATE_RETAINED)) {
                rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
                return EFS_OK;
            }
            /* Extra-shard owner is still assembling pages after restart.
             * Same target — do not flip to another node. */
            unsigned long long sleep_start = rpc_prof_now_us();
            if (attempt == 15 || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        if (r->status == EFS_INODE_RPC_STALE && stale_retryable(type)) {
            /* PREPARE can report the participant leader whose canonical
             * version disagreed with this replica. Re-reading repeatedly on
             * the same coordinator cannot resolve that replica-local drift. */
            target = r->primary_id && r->primary_id <= EFS_MAX_NODES &&
                     r->primary_id != nid ? r->primary_id : 0;
            skip = target ? 0 : nid;
            /* A directory-op txn lands STALE when a same-name dentry, the
             * child row, or an emptiness witness moved between its read
             * and its PREPARE (the parent row itself is a commutative
             * reduce since §7.2 and never conflicts). Nothing committed;
             * re-running re-evaluates against the new state (EEXIST /
             * ENOENT / ENOTEMPTY as appropriate), so retry like BUSY.
             * REPORT_CHUNKS owns its STALE (W1 refetch+overlay) — never
             * retried here. */
            saw_busy = 1;
            unsigned long long sleep_start = rpc_prof_now_us();
            if (attempt == 15 || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
        if (t3 > t2 && t3 - t2 > 10000)
            fprintf(stderr, "inode-rpc: slow-recv type=%u us=%llu\n",
                    type, t3 - t2);
        if (r->status != EFS_INODE_RPC_NOT_PRIMARY) {
            rpc_note_slow_ok(type, attempt, saw_busy, r->status, t_start);
            return EFS_OK;
        }
        /* NOT_PRIMARY: retry on the server-reported primary. No hint (or
         * the hint is the node that just answered) = an election is in
         * progress or a stale leader just stepped down and has not heard
         * the new one yet (0.5-1 s). That is transient, same as BUSY:
         * back off and re-pick, do not fail. Failing here turned into
         * ENOENT/EIO at every caller (stat_ino maps any rc to NOT_FOUND),
         * e.g. GETATTR of a just-made dir right after a leader freeze. */
        if (r->primary_id == 0 || r->primary_id == nid) {
            unsigned long long sleep_start = rpc_prof_now_us();
            saw_busy = 1;
            target = 0;
            skip = nid;
            /* The noted leader no longer is one: forget it so the
             * re-pick does not land here again. */
            if (rpc_leader_of(efs_raft_shard_group(shard)) == nid)
                __atomic_store_n(&g_group_leader[efs_raft_shard_group(shard) >> 1],
                                 0, __ATOMIC_RELAXED);
            fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=no-hint from=%u\n",
                    type, attempt, nid);
            if (attempt == 15 || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2,
                         rpc_prof_now_us() - sleep_start);
            continue;
        }
        target = r->primary_id;
    }
    /* Exhausted BUSY retries is not "no primary" — that mapped to EIO
     * on dir-rename under load (isolated PASS). Say so: an app-visible
     * EBUSY after 10 s of retries on an idle cluster is a server bug and
     * the server line (raft-host: mkdir/create/unlink ... rc=) is the pair. */
    if (saw_busy)
        fprintf(stderr, "inode-rpc: shard=%u type=%u exhausted 16 BUSY/STALE "
                "retries (%.1f s) -> EBUSY\n", shard, type, 10.35);
    return saw_busy ? EFS_ERR_BUSY : EFS_ERR_NOT_PRIMARY;
}

static int rpc_send_recv_owner(efs_ino_t ino, uint8_t type, const void *req,
                               uint32_t req_len, uint8_t expect, void *reply,
                               uint32_t reply_len)
{
    /* The KV shard space is the fixed 12-bit one (ino & 0xFFF). */
    return rpc_send_recv_shard(efs_export_shard_of(ino, EFS_KV_SHARD_BITS),
                               type, req, req_len, expect, reply, reply_len,
                               0, NULL);
}

int efs_client_rpc_lookup_path(efs_export_id_t export_id, efs_ino_t start,
                               const char *path, uint32_t flags,
                               struct efs_msg_inode_lookup_path_reply *out)
{
    struct efs_msg_inode_lookup_path req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.flags = flags;
    req.start = start ? start : EFS_ROOT_INO;
    if (path)
        strncpy(req.path, path, sizeof(req.path) - 1);
    struct efs_msg_inode_lookup_path_reply r;
    int rc = rpc_send_recv_owner(req.start, EFS_MSG_INODE_LOOKUP_PATH, &req,
                                 sizeof(req), EFS_MSG_INODE_LOOKUP_PATH_REPLY,
                                 &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r;
    return EFS_OK;
}

int efs_client_rpc_lookup(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, struct efs_inode *out)
{
    struct efs_msg_inode_lookup req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    if (name)
        strncpy(req.name, name, EFS_MAX_NAME - 1);
    struct efs_msg_inode_reply r;
    /* The raft host resolves dentry placement (local vs hashed layout)
     * internally and bounces when the answering node lacks a group, so the
     * client always enters at the parent directory's shard. */
    int rc = rpc_send_recv_owner(parent, EFS_MSG_INODE_LOOKUP, &req, sizeof(req),
                                 EFS_MSG_INODE_LOOKUP_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    rpc_learn_leader(&r);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_create(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, uint32_t mode, uid_t uid, gid_t gid,
                          uint32_t flags, efs_ino_t *out_ino,
                          struct efs_inode *out)
{
    uint8_t buf[sizeof(struct efs_msg_inode_create) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_create *req = (struct efs_msg_inode_create *)buf;
    struct efs_opid_req q;
    uint32_t slen;
    int slot;

    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->parent = parent;
    if (name)
        strncpy(req->name, name, EFS_MAX_NAME - 1);
    req->mode = mode;
    req->uid = (uint32_t)uid;
    req->gid = (uint32_t)gid;
    req->flags = flags;
    if ((flags & EFS_CREATE_F_HOLD) && !g_client.flock_token)
        g_client.flock_token = 1;
    req->owner = g_client.flock_token;
    slot = opid_begin(&q);
    /* A full local receipt window has sent no CREATE and allocated no
     * identity. Wait only inside an existing caller deadline, as retained
     * regular CREATE does; admitted transport retries keep one identity. */
    while (slot == -1 && efs_client_rpc_deadline_ms() &&
           !efs_client_rpc_past_deadline())
        slot = opid_begin(&q);
    if (slot < 0)
        return EFS_ERR_BUSY;
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    /* Files co-locate with the parent (one group). MKDIR scatters the
     * child inode, so a parent-group-only voter has to bounce the whole
     * RPC to a dual-host — an extra RTT on ~half of mkdirs. Send dirs
     * to a dual-host so both groups are local. */
    int rc = S_ISDIR(mode)
                 ? rpc_send_recv_dual(EFS_MSG_INODE_CREATE, buf, slen,
                                      EFS_MSG_INODE_CREATE_REPLY, &r,
                                      sizeof(r), 0)
                 : rpc_send_recv_owner(parent, EFS_MSG_INODE_CREATE, buf,
                                       slen, EFS_MSG_INODE_CREATE_REPLY,
                                       &r, sizeof(r));
    if (getenv("EFS_OPID_TRACE") && S_ISDIR(mode))
        fprintf(stderr,"opid-mkdir: parent=%llu name=%s seq=%llu transport_rc=%d status=%u\n",
            (unsigned long long)parent,name?name:"",(unsigned long long)q.id.seq,
            rc,rc==EFS_OK?r.status:255u);
    opid_end(slot);
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (r.inode.parent != parent ||
        (name && strncmp(r.inode.name, name, EFS_MAX_NAME - 1) != 0))
        fprintf(stderr,
                "rpc-create: REPLY MISMATCH req parent=%llu name=%s -> "
                "got ino=%llu parent=%llu name=%s mode=%o\n",
                (unsigned long long)parent, name ? name : "",
                (unsigned long long)r.inode.ino,
                (unsigned long long)r.inode.parent, r.inode.name,
                r.inode.mode);
    if (out_ino)
        *out_ino = r.inode.ino;
    if (out)
        *out = r.inode;
    return EFS_OK;
}

/* Remember established participant floors, not ACTIVE authority or ticket
 * permission. Every mutation still validates its server-side fence; PUT
 * STATUS/ADMIT additionally checks fresh owner authority on every request. */
int efs_client_session_admit(uint32_t shard,const struct efs_opid *identity)
{
    if(shard>=4096 || !identity || !identity->session_epoch)return EFS_ERR_INVAL;
    unsigned nonzero=0;
    for(unsigned i=0;i<EFS_OPID_UUID_LEN;i++)nonzero|=identity->client_uuid[i];
    if(!nonzero)return EFS_ERR_INVAL;
    uint32_t byte=shard/8;uint8_t bit=(uint8_t)(1u<<(shard%8));
    pthread_mutex_lock(&opid_mu);
    int same=g_client.append_session_epoch==identity->session_epoch &&
        !memcmp(g_client.append_session_uuid,identity->client_uuid,EFS_OPID_UUID_LEN);
    int established=same && (g_client.append_session_shards[byte]&bit);
    pthread_mutex_unlock(&opid_mu);
    if(established)return EFS_OK;
    int rc=efs_client_rpc_publication_session(shard,identity);
    if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(&opid_mu);
    if(g_client.append_session_epoch!=identity->session_epoch ||
       memcmp(g_client.append_session_uuid,identity->client_uuid,EFS_OPID_UUID_LEN)) {
        memset(g_client.append_session_shards,0,sizeof(g_client.append_session_shards));
        memcpy(g_client.append_session_uuid,identity->client_uuid,EFS_OPID_UUID_LEN);
        g_client.append_session_epoch=identity->session_epoch;
    }
    g_client.append_session_shards[byte]|=bit;
    pthread_mutex_unlock(&opid_mu);return EFS_OK;
}

int efs_client_create_begin(struct efs_client_create_request *r,efs_ino_t parent,
                             const char *name,uint32_t mode,uid_t uid,gid_t gid)
{
    if(!r || !parent || !name || !name[0] ||
       strnlen(name,EFS_MAX_NAME)>=EFS_MAX_NAME || !S_ISREG(mode))return EFS_ERR_INVAL;
    if(r->active)return EFS_ERR_BUSY;
    struct efs_opid_req q;int slot=opid_begin(&q);
    if(slot<0)return EFS_ERR_BUSY;
    memset(r,0,sizeof(*r));r->slot=slot;r->active=1;
    r->wire.q=q;r->wire.create.export_id=g_client.export_id;
    r->wire.create.parent=parent;r->wire.create.mode=mode;
    r->wire.create.uid=uid;r->wire.create.gid=gid;
    r->wire.create.flags=EFS_CREATE_F_PENDING;
    memcpy(r->wire.create.name,name,strlen(name)+1);
    return EFS_OK;
}

static int create_request_rpc(struct efs_client_create_request *r,int probe)
{
    struct efs_msg_create_retained request=r->wire;
    request.probe=probe;
    struct efs_msg_create_retained_reply reply={0};
    int rc=rpc_send_recv_owner(request.create.parent,EFS_MSG_CREATE_RETAINED,
        &request,sizeof(request),EFS_MSG_CREATE_RETAINED_REPLY,&reply,sizeof(reply));
    if(rc!=EFS_OK)return rc;
    if(reply.known>1 || reply.routed>1 || (reply.known && !reply.routed) ||
       (reply.routed && reply.shard>=EFS_SESSION_BITS) ||
       (r->wire.routed && reply.routed && r->wire.shard!=reply.shard))return EFS_ERR_PROTO;
    if(reply.routed){r->wire.routed=1;r->wire.shard=reply.shard;}
    rc=rpc_status_to_efs(reply.rpc.status);
    /* A successful submission without a replay receipt is still ambiguous.
     * Keep the same identity/slot and retry until durable terminal proof;
     * returning OK here makes FUSE stop its retry loop before completion. */
    if(!reply.known && rc==EFS_OK)rc=EFS_ERR_BUSY;
    if(reply.known) {
        if(rc==EFS_ERR_BUSY || rc==EFS_ERR_NOT_PRIMARY ||
           (rc==EFS_OK && (!reply.rpc.inode.ino || !reply.generation)))return EFS_ERR_PROTO;
        r->inode=reply.rpc.inode;r->generation=reply.generation;
        r->complete=1;r->result=rc;opid_end(r->slot);r->slot=-1;
    }
    if(r->attempted && !reply.known && getenv("EFS_OPID_TRACE"))
        fprintf(stderr,"opid-create: unresolved seq=%llu parent=%llu shard=%u routed=%u rc=%d probe=%d\n",
            (unsigned long long)r->wire.q.id.seq,(unsigned long long)r->wire.create.parent,
            r->wire.shard,r->wire.routed,rc,probe);
    return rc;
}

static int create_request_revocation(struct efs_client_create_request *r,int rc)
{
    if(!r->complete && (rc==EFS_ERR_STALE || rc==EFS_ERR_NOT_FOUND)) {
        int revoked=0;
        if(efs_client_rpc_identity_revoked(&r->wire.q.id,&revoked)==EFS_OK && revoked) {
            r->revoked=1;r->complete=1;r->result=EFS_ERR_STALE;
            opid_end(r->slot);r->slot=-1;return EFS_ERR_STALE;
        }
    }
    return rc;
}

int efs_client_create_resume(struct efs_client_create_request *r,int probe_only)
{
    if(!r || !r->active || probe_only<0 || probe_only>1)return EFS_ERR_INVAL;
    if(r->complete)return r->result;
    int rc=create_request_rpc(r,1);
    if(r->complete || probe_only || rc!=EFS_ERR_BUSY)return create_request_revocation(r,rc);
    /* The parent read-index may be BUSY before the server can resolve the
     * dentry shard. This is a valid unresolved probe, not malformed proof.
     * Retain the identity and let the caller retry without submitting. */
    if(!r->wire.routed)return EFS_ERR_BUSY;
    if(!r->admitted) {
        rc=efs_client_session_admit(r->wire.shard,&r->wire.q.id);
        if(rc!=EFS_OK)return create_request_revocation(r,rc);
        r->admitted=1;
    }
    opid_refresh_ack(&r->wire.q);
    r->attempted=1;
    return create_request_revocation(r,create_request_rpc(r,0));
}

int efs_client_create_reset(struct efs_client_create_request *r)
{
    if(!r)return EFS_ERR_INVAL;
    if(r->active && !r->complete)return EFS_ERR_BUSY;
    memset(r,0,sizeof(*r));return EFS_OK;
}
int efs_client_create_cancel(struct efs_client_create_request *r)
{
    if(!r)return EFS_ERR_INVAL;
    if(r->active && r->attempted && !r->complete)return EFS_ERR_BUSY;
    if(r->active && !r->complete)opid_end(r->slot);
    memset(r,0,sizeof(*r));return EFS_OK;
}

int efs_client_rpc_getattr(efs_export_id_t export_id, efs_ino_t ino,
                           struct efs_inode *out)
{
    struct efs_msg_inode_getattr req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_GETATTR, &req, sizeof(req),
                                 EFS_MSG_INODE_GETATTR_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    rpc_learn_leader(&r);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

/* Raft-mode readdir: the KV scans a dir in NAME order, so pagination uses
 * the server's (src, name) resume cookie, not an ino cursor (name order !=
 * ino order — an ino filter skips entries). src_io/name_io are in/out:
 * pass 0/"" for the first page, then the previous reply's cookie verbatim.
 * done_out=1 when the scan is exhausted. */
int efs_client_rpc_readdir_cur(efs_export_id_t export_id, efs_ino_t parent,
                               struct efs_inode *ents, uint32_t *inout_count,
                               uint32_t *src_io, char *name_io,
                               uint32_t *done_out)
{
    struct efs_msg_inode_readdir req;
    struct efs_conn *conn;
    efs_node_id_t nid = 0;
    uint8_t rtype = 0;
    void *payload = NULL;
    uint32_t plen = 0;
    uint32_t shard = efs_kv_inode_shard(parent);
    int rc;

    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    if (inout_count && !*inout_count)
        return EFS_ERR_INVAL;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    req.max_ents = inout_count ? *inout_count : EFS_READDIR_MAX;
    if (req.max_ents > EFS_READDIR_MAX)
        req.max_ents = EFS_READDIR_MAX;
    req.after_src = src_io ? *src_io : 0;
    if (name_io)
        strncpy(req.after_name, name_io, EFS_MAX_NAME - 1);
    int prof = rpc_prof_enabled();
    unsigned long long t0 = prof ? rpc_prof_now_us() : 0;
    conn = rpc_owner_conn_shard(shard, &nid);
    unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
    if (!conn)
        return EFS_ERR_NET;
    if (efs_conn_send_msg(conn, EFS_MSG_INODE_READDIR, &req, sizeof(req)) != 0) {
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
    rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
    unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
    if (rc != 0) {
        free(payload);
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    efs_client_conn_release(nid, conn);
    if (efs_client_rpc_past_deadline()) {
        free(payload);
        return EFS_ERR_BUSY;
    }
    rpc_prof_add(EFS_MSG_INODE_READDIR, t1 - t0, t2 - t1, t3 - t2, 0);
    if (rtype != EFS_MSG_INODE_READDIR_REPLY ||
        plen < sizeof(struct efs_msg_inode_readdir_reply)) {
        free(payload);
        return EFS_ERR_PROTO;
    }
    {
        struct efs_msg_inode_readdir_reply *r = payload;
        uint32_t n;
        if (r->status != EFS_INODE_RPC_OK) {
            int st = rpc_status_to_efs(r->status);
            free(payload);
            return st;
        }
        if (r->count > req.max_ents || r->next_done > 1 ||
            !memchr(r->next_name, 0, sizeof(r->next_name)) ||
            (!r->next_done && r->next_src == req.after_src &&
             !strcmp(r->next_name, req.after_name))) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        n = r->count;
        if (inout_count && n > *inout_count)
            n = *inout_count;
        if (ents && n)
            memcpy(ents, r->ents, n * sizeof(ents[0]));
        if (inout_count)
            *inout_count = n;
        if (src_io)
            *src_io = r->next_src;
        if (name_io) {
            strncpy(name_io, r->next_name, EFS_MAX_NAME - 1);
            name_io[EFS_MAX_NAME - 1] = 0;
        }
        if (done_out)
            *done_out = r->next_done;
    }
    free(payload);
    return EFS_OK;
}

int efs_client_rpc_writer_view(efs_ino_t ino, uint64_t gen, uint32_t ci,
                                struct efs_msg_inode_writer_view_reply *out)
{
    if (!ino || !out)
        return EFS_ERR_INVAL;
    struct efs_msg_inode_writer_view req = {ino, gen, ci, 0};
    uint32_t shard = efs_kv_inode_shard(ino);
    efs_node_id_t target = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = target;
        struct efs_conn *conn = target ? efs_client_conn_get(target) :
                                         rpc_owner_conn_shard(shard, &nid);
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, EFS_MSG_INODE_WRITER_VIEW, &req, sizeof(req))) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        uint8_t type = 0;
        uint32_t len = 0;
        void *payload = NULL;
        if (efs_conn_recv_msg(conn, &type, &payload, &len)) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        if (type != EFS_MSG_INODE_WRITER_VIEW_REPLY || len != sizeof(*out)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        struct efs_msg_inode_writer_view_reply reply;
        memcpy(&reply, payload, sizeof(reply));
        free(payload);
        if (reply.status == EFS_INODE_RPC_NOT_PRIMARY && reply.primary_id &&
            reply.primary_id != nid) {
            target = reply.primary_id;
            continue;
        }
        if (reply.status == EFS_INODE_RPC_BUSY) {
            if (attempt == 7 || rpc_writer_retry_pause(attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        int rc = rpc_status_to_efs(reply.status);
        if (rc != EFS_OK)
            return rc;
        rc = efs_writer_view_reply_valid(&req, &reply);
        if (rc == EFS_OK)
            *out = reply;
        return rc;
    }
    return EFS_ERR_BUSY;
}

static int rpc_writer_retry_pause(unsigned attempt)
{
    unsigned delay = 50000u << (attempt < 4 ? attempt : 4);
    uint64_t deadline = efs_client_rpc_deadline_ms();
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    if (deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
        if (now >= deadline)
            return EFS_ERR_BUSY;
        uint64_t remaining = deadline - now;
        if (remaining < (delay + 999u) / 1000u)
            delay = (unsigned)(remaining * 1000u);
    }
    usleep(delay);
    return efs_client_rpc_past_deadline() ? EFS_ERR_BUSY : EFS_OK;
}

/* Raw session replies carry an EFS rc, not an inode status byte. Keep this
 * separate from rpc_send_recv_shard so a Raft rc cannot be misinterpreted. */
static int session_command_reply(uint32_t shard,const uint8_t *command,uint32_t bytes,
    uint64_t *known,int probe)
{
    uint8_t body[EFS_MD_SESS_LEASE_EDGE_LEN+1];
    if(known)*known=0;
    if (bytes>sizeof(body)-1) return EFS_ERR_INVAL;
    if(probe==2 && (!known || bytes!=22 || command[0]!=EFS_MD_CMD_SESSION ||
                    command[1]!=0))return EFS_ERR_INVAL;
    body[0]=efs_raft_shard_group(shard);memcpy(body+1,command,bytes);
    efs_node_id_t target=0;
    for (unsigned attempt=0;attempt<8;attempt++) {
        if (efs_client_rpc_past_deadline()) return EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):rpc_owner_conn_shard(shard,&nid);
        if (!conn) return EFS_ERR_NET;
        if (efs_conn_send_msg(conn,EFS_MSG_RAFT_MKFS,body,bytes+1)) {
            efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        uint8_t type=0;uint32_t len=0;void *payload=NULL;
        if (efs_conn_recv_msg(conn,&type,&payload,&len)) {
            free(payload);efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        efs_client_conn_release(nid,conn);
        if (type!=EFS_MSG_RAFT_MKFS_REPLY || len!=sizeof(struct efs_msg_raft_mkfs_reply)) {
            free(payload);return EFS_ERR_PROTO;
        }
        struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,payload,sizeof(reply));free(payload);
        if (efs_client_rpc_past_deadline()) return EFS_ERR_BUSY;
        /* Session/lease commands name their authoritative group as inode
         * replies do. Retain only a validated successful routing hint; this
         * cache is never session authority or proof of a stored verdict. */
        if (reply.rc==EFS_OK && reply.leader_hint>=0 &&
            reply.leader_hint<EFS_MAX_NODES)
            rpc_note_leader(body[0],(efs_node_id_t)(reply.leader_hint+1));
        if(probe==2 && reply.rc==EFS_OK){*known=reply.salt;return EFS_OK;}
        if(known && probe!=2) {
            if(reply.salt>1 || (reply.salt &&
               (reply.rc==EFS_ERR_NOT_PRIMARY || reply.rc==EFS_ERR_NO_QUORUM)))
                return EFS_ERR_PROTO;
            if(reply.salt) {*known=1;return reply.rc;}
            if(reply.rc==EFS_OK)return EFS_ERR_PROTO; /* no proof of stored result */
            if(probe && reply.rc==EFS_ERR_BUSY)return EFS_ERR_BUSY;
        }
        if (reply.rc==EFS_ERR_NOT_PRIMARY) {
            if (reply.leader_hint < -1 || reply.leader_hint >= EFS_MAX_NODES) return EFS_ERR_PROTO;
            target=reply.leader_hint>=0 && (efs_node_id_t)(reply.leader_hint+1)!=nid ?
                (efs_node_id_t)(reply.leader_hint+1):0;
        } else if (reply.rc!=EFS_ERR_BUSY && reply.rc!=EFS_ERR_NO_QUORUM) return reply.rc;
        if (rpc_writer_retry_pause(attempt)!=EFS_OK) return EFS_ERR_BUSY;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_identity_revoked(const struct efs_opid *id,int *revoked)
{
    if(!revoked)return EFS_ERR_INVAL;
    *revoked=0;
    if(!id || !id->session_epoch)return EFS_ERR_INVAL;
    unsigned nonzero=0;for(unsigned i=0;i<EFS_OPID_UUID_LEN;i++)nonzero|=id->client_uuid[i];
    if(!nonzero)return EFS_ERR_INVAL;
    uint8_t query[22]={EFS_MD_CMD_SESSION,0};
    memcpy(query+2,id->client_uuid,EFS_OPID_UUID_LEN);
    uint64_t view=0;
    int rc=session_command_reply(efs_kv_session_shard(id->client_uuid),query,sizeof(query),&view,2);
    if(rc!=EFS_OK)return rc;
    uint32_t epoch=(uint32_t)view;unsigned state=(unsigned)((view>>32)&255u);
    if((view>>41) || !epoch || (state!=EFS_SESSION_ACTIVE && state!=EFS_SESSION_FENCING))
        return EFS_ERR_PROTO;
    *revoked=state==EFS_SESSION_ACTIVE && epoch>id->session_epoch;
    return EFS_OK;
}

static int publication_session_command(uint32_t shard,const uint8_t *command,uint32_t bytes)
{return session_command_reply(shard,command,bytes,NULL,0);}

static void lease_command_be(uint8_t *out,uint64_t value,unsigned bytes)
{while(bytes){out[--bytes]=(uint8_t)value;value>>=8;}}

/* Identity, generation and body belong to the caller across ambiguity. Probe
 * never registers a session or invents an edge. known=1 is durable result proof;
 * an ordinary error with known=0 is not permission to forget pending ownership. */
int efs_client_rpc_lease_edge(efs_ino_t ino,uint64_t generation,
    const struct efs_opid_req *q,unsigned flags,uint64_t pending_token,
    int probe,uint64_t *known)
{
    if(!known)return EFS_ERR_INVAL;
    *known=0;
    if(!ino || !generation || !q || !efs_opid_req_valid(q) ||
       !q->id.session_epoch || q->ack>=q->id.seq || probe<0 || probe>1 ||
       !flags || (flags&~7u) || (flags&3u)==3u ||
       !!(flags&EFS_LEASE_EDGE_CLEAR_PENDING)!=!!pending_token)return EFS_ERR_INVAL;
    uint8_t command[EFS_MD_SESS_LEASE_EDGE_LEN]={EFS_MD_CMD_SESSION,
        probe?EFS_MD_SESS_LEASE_PROBE_DEFERRED:EFS_MD_SESS_LEASE_EDGE_DEFERRED};
    memcpy(command+2,q->id.client_uuid,EFS_OPID_UUID_LEN);
    lease_command_be(command+18,q->id.session_epoch,4);
    lease_command_be(command+22,q->id.seq,8);lease_command_be(command+30,q->ack,8);
    lease_command_be(command+38,ino,8);lease_command_be(command+46,generation,8);
    lease_command_be(command+54,flags,4);lease_command_be(command+58,pending_token,8);
    return session_command_reply(efs_kv_inode_shard(ino),command,sizeof(command),known,probe);
}

/* Call before byte admission. CREATE does not reset an existing session;
 * REGISTER and host-side ESTABLISH check its authoritative epoch/fence.
 * Retry the same UUID/epoch after an ambiguous reply, never mint a replacement
 * identity to bypass a fence. Queries and retirement do not call this helper. */
int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *identity)
{
    if (!identity || shard>=4096 || !identity->session_epoch) return EFS_ERR_INVAL;
    unsigned nonzero=0;
    for (unsigned i=0;i<EFS_OPID_UUID_LEN;i++) nonzero|=identity->client_uuid[i];
    if (!nonzero) return EFS_ERR_INVAL;
    uint8_t command[26]={EFS_MD_CMD_SESSION,0};
    memcpy(command+2,identity->client_uuid,EFS_OPID_UUID_LEN);
    lease_command_be(command+18,shard,4);
    uint32_t owner=efs_kv_session_shard(identity->client_uuid);
    uint64_t view=0;
    int rc=session_command_reply(owner,command,22,&view,2);
    int registered=0;
    if(rc==EFS_OK) {
        uint32_t epoch=(uint32_t)view;unsigned state=(unsigned)((view>>32)&255u);
        if((view>>41) || !epoch || (state!=EFS_SESSION_ACTIVE && state!=EFS_SESSION_FENCING))return EFS_ERR_PROTO;
        if(epoch!=identity->session_epoch)return EFS_ERR_STALE;
        if(state==EFS_SESSION_FENCING)return EFS_ERR_BUSY;
        registered=!!(view&(1ull<<40));
    } else if(rc==EFS_ERR_NOT_FOUND) {
        command[1]=EFS_MD_SESS_CREATE;
        lease_command_be(command+18,identity->session_epoch,4);
        rc=publication_session_command(owner,command,22);
        if(rc)return rc;
    } else return rc;
    lease_command_be(command+18,identity->session_epoch,4);
    lease_command_be(command+22,shard,4);
    if(!registered && efs_raft_shard_group(owner)==efs_raft_shard_group(shard)) {
        /* One replicated transition checks the owner epoch and participant
         * rejection floor atomically. Cross-group admission must still register
         * durably before establishing the participant. */
        command[1]=EFS_MD_SESS_REGISTER_ESTABLISH;
        return publication_session_command(shard,command,sizeof(command));
    }
    if(!registered) {
        command[1]=EFS_MD_SESS_REGISTER;
        rc=publication_session_command(owner,command,sizeof(command));
        if(rc)return rc;
    }
    /* ESTABLISH always rechecks fresh owner authority and the participant's
     * durable rejection floor. This is an RPC-local proof, not a client cache. */
    command[1]=EFS_MD_SESS_ESTABLISH;
    return publication_session_command(shard,command,sizeof(command));
}

/* Production ownership primitive: an unknown reply never frees its ACK slot
 * or permits replacing identity/body. Namespace and publication use the same
 * full mount UUID; legacy append/HOLD still need a versioned adoption bridge. */
int efs_client_lease_begin(struct efs_client_lease_request *r,efs_ino_t ino,
                           uint64_t generation,unsigned flags,uint64_t token)
{
    if (!r || !ino || !generation || !flags || (flags & ~7u) ||
        (flags & 3u) == 3u ||
        !!(flags & EFS_LEASE_EDGE_CLEAR_PENDING) != !!token)
        return EFS_ERR_INVAL;
    if (r->active) return EFS_ERR_BUSY;
    struct efs_opid_req q;
    int slot = opid_begin(&q);
    if (slot < 0) return EFS_ERR_BUSY;
    *r = (struct efs_client_lease_request){.q=q,.ino=ino,
        .generation=generation,.pending_token=token,.flags=flags,
        .slot=slot,.active=1};
#if EFS_FAULTS
    /* Corrupt only the guarded request's FileID before its first send. The
     * caller retains CREATE's correct generation for pending-only cleanup. */
    const char *marker=getenv("EFS_FAULT_CREATE_ADOPT_NEGATIVE_FILE");
    if (flags==(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING) &&
        marker && unlink(marker)==0) {
        r->generation=generation==UINT64_MAX ? generation-1 : generation+1;
        fprintf(stderr,"test-create-adopt-negative-request ino=%llu generation=%llu original=%llu\n",
            (unsigned long long)ino,(unsigned long long)r->generation,(unsigned long long)generation);
    }
#endif
    return EFS_OK;
}

static int lease_request_result(struct efs_client_lease_request *r,int rc,
                                 uint64_t known)
{
    if(!known && getenv("EFS_OPID_TRACE"))
        fprintf(stderr,"opid-lease: unresolved seq=%llu ino=%llu flags=%u rc=%d\n",
            (unsigned long long)r->q.id.seq,(unsigned long long)r->ino,r->flags,rc);
    if (known == 1) {
        r->complete=1;
        r->result=rc;
        opid_end(r->slot);
        r->slot=-1;
    }
    if(!r->complete && (rc==EFS_ERR_STALE || rc==EFS_ERR_NOT_FOUND)) {
        int revoked=0;
        if(efs_client_rpc_identity_revoked(&r->q.id,&revoked)==EFS_OK && revoked) {
            r->revoked=1;r->complete=1;r->result=EFS_ERR_STALE;
            opid_end(r->slot);r->slot=-1;return EFS_ERR_STALE;
        }
    }
    return rc;
}

int efs_client_lease_resume(struct efs_client_lease_request *r,int probe_only)
{
    if (!r || !r->active || probe_only<0 || probe_only>1)
        return EFS_ERR_INVAL;
    if (r->complete) return r->result;
    uint64_t known=0;
    int rc;
    if (r->attempted || probe_only) {
        rc=efs_client_rpc_lease_edge(r->ino,r->generation,&r->q,r->flags,
                                     r->pending_token,1,&known);
        if (known || probe_only || rc!=EFS_ERR_BUSY)
            return lease_request_result(r,rc,known);
        /* Unknown probe is not success; retry only the retained exact edge. */
    }
    if (!r->admitted) {
        /* Reuse only a completed establishment for this exact UUID, epoch
         * and shard. The guarded edge still validates server revocation. */
        rc=efs_client_session_admit(efs_kv_inode_shard(r->ino),&r->q.id);
        if (rc!=EFS_OK) return lease_request_result(r,rc,0);
        r->admitted=1;
    }
    opid_refresh_ack(&r->q);
    r->attempted=1;
    rc=efs_client_rpc_lease_edge(r->ino,r->generation,&r->q,r->flags,
                                 r->pending_token,0,&known);
#if EFS_FAULTS
    /* Private functional gate: hide one consumed durable combined-adoption
     * reply. Leave the exact request active so normal cleanup must prove its
     * receipt before compensating CLOSE; no descriptor was acknowledged. */
    const char *marker=getenv("EFS_FAULT_CREATE_ADOPT_REPLY_FILE");
    if (rc==EFS_OK && known==1 &&
        r->flags==(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING) &&
        marker && unlink(marker)==0) {
        fprintf(stderr,"test-create-adopt-reply-loss seq=%llu ino=%llu known=1 hidden=1\n",
                (unsigned long long)r->q.id.seq,(unsigned long long)r->ino);
        return lease_request_result(r,EFS_ERR_NET,0);
    }
#endif
    return lease_request_result(r,rc,known);
}

int efs_client_lease_reset(struct efs_client_lease_request *r)
{
    if (!r) return EFS_ERR_INVAL;
    if (r->active && !r->complete) return EFS_ERR_BUSY;
    memset(r,0,sizeof(*r));
    return EFS_OK;
}

static int rpc_put_ticket(const struct efs_put_ticket *ticket,int probe_only,
                                uint32_t *state)
{
    if(!state)return EFS_ERR_INVAL;
    *state=0;
    if(probe_only<0 || probe_only>2)return EFS_ERR_INVAL;
    uint8_t body[2+EFS_PUT_TICKET_WIRE_LEN];
    int rc=efs_put_ticket_encode(ticket,body+2);if(rc!=EFS_OK)return rc;
    uint32_t shard=efs_kv_lane_shard(ticket->ino,ticket->ci%EFS_META_LANES);
    body[0]=efs_raft_shard_group(shard);
    body[1]=probe_only==2?EFS_MD_CMD_PUT_TICKET_RETIRE_PUBLISHED:probe_only?EFS_MD_REQ_PUT_TICKET_STATUS:EFS_MD_CMD_PUT_TICKET_ADMIT;
    efs_node_id_t target=0;
    for(unsigned attempt=0;attempt<8;attempt++) {
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):rpc_owner_conn_shard(shard,&nid);
        if(!conn)return EFS_ERR_NET;
        if(efs_conn_send_msg(conn,EFS_MSG_RAFT_MKFS,body,sizeof(body))) {
            efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        uint8_t type=0;uint32_t bytes=0;void *payload=NULL;
        if(efs_conn_recv_msg(conn,&type,&payload,&bytes)) {
            free(payload);efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        efs_client_conn_release(nid,conn);
        if(type!=EFS_MSG_RAFT_MKFS_REPLY || bytes!=sizeof(struct efs_msg_raft_mkfs_reply)) {
            free(payload);return EFS_ERR_PROTO;
        }
        struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,payload,bytes);free(payload);
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        if(reply.rc==EFS_OK) {
            if(probe_only==2 ? reply.salt!=EFS_PUT_TICKET_RETIRED :
               (reply.salt<EFS_PUT_TICKET_ADMITTED || reply.salt>EFS_PUT_TICKET_RECLAIMING))
                return EFS_ERR_PROTO;
            if(!probe_only && reply.salt!=EFS_PUT_TICKET_ADMITTED)return EFS_ERR_STALE;
            *state=(uint32_t)reply.salt;return EFS_OK;
        }
        if(reply.salt)return EFS_ERR_PROTO;
        if(reply.rc==EFS_ERR_NOT_PRIMARY) {
            if(reply.leader_hint< -1 || reply.leader_hint>=EFS_MAX_NODES)return EFS_ERR_PROTO;
            target=reply.leader_hint>=0 && (efs_node_id_t)(reply.leader_hint+1)!=nid?
                (efs_node_id_t)(reply.leader_hint+1):0;
        } else if(reply.rc!=EFS_ERR_BUSY && reply.rc!=EFS_ERR_NO_QUORUM)return reply.rc;
        if(rpc_writer_retry_pause(attempt)!=EFS_OK)return EFS_ERR_BUSY;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_put_ticket(const struct efs_put_ticket *ticket,int probe_only,uint32_t *state)
{
    if (!state) return EFS_ERR_INVAL;
    *state=0;
    return probe_only<0 || probe_only>1 ? EFS_ERR_INVAL : rpc_put_ticket(ticket,probe_only,state);
}
int efs_client_rpc_put_ticket_retire(const struct efs_put_ticket *ticket)
{
    uint32_t state=0;
    return rpc_put_ticket(ticket,2,&state);
}

int efs_client_rpc_publication(const struct efs_msg_publication *request, int query_only,
                               struct efs_msg_publication_reply *out)
{
    struct efs_meta_pub p;uint8_t digest[EFS_HASH_SIZE];
    if(query_only<0 || query_only>2 || !out || !request)return EFS_ERR_INVAL;
    struct efs_msg_publication req=*request;
    memset(out,0,sizeof(*out));
    if(efs_publication_from_request(&req,&p)!=EFS_OK)return EFS_ERR_INVAL;
    if(efs_publication_digest(&p,digest)!=EFS_OK)return EFS_ERR_INVAL;
    uint32_t shard=efs_kv_lane_shard(req.rec.ino,req.rec.chunk_index%EFS_META_LANES);
    efs_node_id_t target=0;
    for(unsigned attempt=0;attempt<8;attempt++) {
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):rpc_owner_conn_shard(shard,&nid);
        if(!conn)return EFS_ERR_NET;
        uint8_t msg=query_only==2?EFS_MSG_PUBLICATION_RETIRE:query_only?EFS_MSG_PUBLICATION_STATUS:EFS_MSG_PUBLICATION;
        if(efs_conn_send_msg(conn,msg,&req,sizeof(req))) {
            efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        uint8_t type=0;uint32_t len=0;void *payload=NULL;
        if(efs_conn_recv_msg(conn,&type,&payload,&len)) {
            free(payload);efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        efs_client_conn_release(nid,conn);
        if(type!=(query_only==2?EFS_MSG_PUBLICATION_RETIRE_REPLY:query_only?EFS_MSG_PUBLICATION_STATUS_REPLY:EFS_MSG_PUBLICATION_REPLY) || len!=sizeof(*out)) {
            free(payload);return EFS_ERR_PROTO;
        }
        struct efs_msg_publication_reply reply;memcpy(&reply,payload,sizeof(reply));free(payload);
        if(reply.rpc.status==EFS_INODE_RPC_NOT_PRIMARY && reply.rpc.primary_id && reply.rpc.primary_id!=nid) {
            target=reply.rpc.primary_id;continue;
        }
        if(reply.rpc.status==EFS_INODE_RPC_BUSY) {
            if(rpc_writer_retry_pause(attempt)!=EFS_OK)return EFS_ERR_BUSY;
            continue;
        }
        int rc=rpc_status_to_efs(reply.rpc.status);if(rc!=EFS_OK)return rc;
        if ((reply.receipt_index && (!reply.receipt_term ||
             reply.receipt_group!=efs_raft_shard_group(shard))) ||
            (!reply.receipt_index && (reply.receipt_term || reply.receipt_group)))
            return EFS_ERR_PROTO;
        if(memcmp(digest,reply.digest,sizeof(digest)) ||
           (reply.state!=EFS_PUBLICATION_COMMITTED && reply.state!=EFS_PUBLICATION_REJECTED && reply.state!=EFS_PUBLICATION_RETIRED && reply.state!=EFS_PUBLICATION_PENDING && reply.state!=EFS_PUBLICATION_APPLY_PENDING) ||
           (reply.state==EFS_PUBLICATION_RETIRED && reply.verdict!=EFS_META_PUBLICATION_RETIRED) ||
           ((reply.state==EFS_PUBLICATION_PENDING || reply.state==EFS_PUBLICATION_APPLY_PENDING) && reply.verdict!=EFS_META_PUBLICATION_PENDING) ||
           (reply.state==EFS_PUBLICATION_APPLY_PENDING && !reply.receipt_index) ||
           (query_only==2 && reply.state!=EFS_PUBLICATION_RETIRED && reply.state!=EFS_PUBLICATION_PENDING) ||
           (reply.state==EFS_PUBLICATION_COMMITTED && reply.verdict!=EFS_OK) ||
           (reply.state==EFS_PUBLICATION_REJECTED && reply.verdict!=EFS_ERR_STALE && reply.verdict!=EFS_ERR_INVAL))
            return EFS_ERR_PROTO;
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        *out=reply;return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_publication_batch(const struct efs_msg_publication_batch *req,
                                     struct efs_msg_publication_batch_reply *out)
{
    uint8_t digests[EFS_PUBLICATION_BATCH_MAX][EFS_HASH_SIZE];
    if(!req || !out)return EFS_ERR_INVAL;
    memset(out,0,sizeof(*out));
    if(req->version!=EFS_PUBLICATION_BATCH_VERSION || !req->count ||
       req->count>EFS_PUBLICATION_BATCH_MAX || (req->mode!=0 && req->mode!=2) ||
       (req->group!=0 && req->group!=2))return EFS_ERR_INVAL;
    for(uint32_t i=0;i<req->count;i++) {
        struct efs_meta_pub p;
        if(efs_publication_from_request(&req->records[i],&p) || p.chunk_index ||
           p.coding_profile_id!=EFS_META_PROFILE_PACK_K2F1 ||
           efs_raft_shard_group(efs_kv_lane_shard(p.ino,0))!=req->group ||
           efs_publication_digest(&p,digests[i]))return EFS_ERR_INVAL;
        for(uint32_t j=0;j<i;j++)if(req->records[j].rec.ino==p.ino)return EFS_ERR_INVAL;
    }
    uint32_t shard=efs_kv_lane_shard(req->records[0].rec.ino,0);
    efs_node_id_t target=0;
    for(unsigned attempt=0;attempt<8;attempt++) {
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):rpc_owner_conn_shard(shard,&nid);
        if(!conn)return EFS_ERR_NET;
        if(efs_conn_send_msg(conn,EFS_MSG_PUBLICATION_BATCH,req,(uint32_t)efs_publication_batch_bytes(req->count))) {
            efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        uint8_t type=0;uint32_t len=0;void *payload=NULL;
        if(efs_conn_recv_msg(conn,&type,&payload,&len)) {
            free(payload);efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        efs_client_conn_release(nid,conn);
        if(type!=EFS_MSG_PUBLICATION_BATCH_REPLY || len<offsetof(struct efs_msg_publication_batch_reply,records)) {
            free(payload);return EFS_ERR_PROTO;
        }
        struct efs_msg_publication_batch_reply reply={0};
        if(len>sizeof(reply)){free(payload);return EFS_ERR_PROTO;}
        memcpy(&reply,payload,len);free(payload);
        if(reply.rpc.status==EFS_INODE_RPC_NOT_PRIMARY && reply.rpc.primary_id && reply.rpc.primary_id!=nid) {
            target=reply.rpc.primary_id;continue;
        }
        if(reply.rpc.status==EFS_INODE_RPC_BUSY || reply.rpc.status==EFS_INODE_RPC_NOT_PRIMARY) {
            if(rpc_writer_retry_pause(attempt))return EFS_ERR_BUSY;
            continue;
        }
        int rc=rpc_status_to_efs(reply.rpc.status);if(rc)return rc;
        if(reply.version!=req->version || reply.mode!=req->mode || reply.group!=req->group ||
           reply.count!=req->count || len!=efs_publication_batch_reply_bytes(req->count))return EFS_ERR_PROTO;
        /* Validate the ENTIRE frame before exposing any record to the cache.
         * An error item is explicit uncertainty, not an omitted success. */
        for(uint32_t i=0;i<req->count;i++) {
            const struct efs_msg_publication_reply *r=&reply.records[i];
            if(memcmp(digests[i],r->digest,EFS_HASH_SIZE))return EFS_ERR_PROTO;
            if(r->rpc.status!=EFS_INODE_RPC_OK) {
                if(r->state || r->receipt_index || r->receipt_term || r->receipt_group)return EFS_ERR_PROTO;
                continue;
            }
            if((r->receipt_index && (!r->receipt_term || r->receipt_group!=req->group)) ||
               (!r->receipt_index && (r->receipt_term || r->receipt_group)) ||
               (r->state!=EFS_PUBLICATION_COMMITTED && r->state!=EFS_PUBLICATION_REJECTED &&
                r->state!=EFS_PUBLICATION_RETIRED && r->state!=EFS_PUBLICATION_PENDING) ||
               (r->state==EFS_PUBLICATION_COMMITTED && r->verdict!=EFS_OK) ||
               (r->state==EFS_PUBLICATION_REJECTED && r->verdict!=EFS_ERR_STALE && r->verdict!=EFS_ERR_INVAL) ||
               (r->state==EFS_PUBLICATION_PENDING && r->verdict!=EFS_META_PUBLICATION_PENDING) ||
               (r->state==EFS_PUBLICATION_RETIRED && r->verdict!=EFS_META_PUBLICATION_RETIRED) ||
               (req->mode==2 && r->state!=EFS_PUBLICATION_RETIRED))return EFS_ERR_PROTO;
        }
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        *out=reply;return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_lane_writer_view(efs_ino_t ino, uint64_t gen, uint32_t ci, uint32_t cs,
                                struct efs_msg_lane_writer_view_reply *out)
{
    if (!ino || !gen || !out || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    struct efs_msg_lane_writer_view req = {ino, gen, ci, cs};
    uint32_t shard = efs_kv_lane_shard(ino, ci % 64u);
    efs_node_id_t target = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = target;
        struct efs_conn *conn = target ? efs_client_conn_get(target) :
                                         rpc_owner_conn_shard(shard, &nid);
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, EFS_MSG_LANE_WRITER_VIEW, &req, sizeof(req))) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        uint8_t type = 0;
        uint32_t len = 0;
        void *payload = NULL;
        if (efs_conn_recv_msg(conn, &type, &payload, &len)) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        if (type != EFS_MSG_LANE_WRITER_VIEW_REPLY || len != sizeof(*out)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        struct efs_msg_lane_writer_view_reply reply;
        memcpy(&reply, payload, sizeof(reply));
        free(payload);
        if (reply.view.status == EFS_INODE_RPC_NOT_PRIMARY && reply.view.primary_id &&
            reply.view.primary_id != nid) {
            target = reply.view.primary_id;
            continue;
        }
        if (reply.view.status == EFS_INODE_RPC_BUSY) {
            if (rpc_writer_retry_pause(attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        int rc = rpc_status_to_efs(reply.view.status);
        if (rc != EFS_OK)
            return rc;
        rc = efs_lane_writer_view_reply_valid(&req, &reply);
        if (rc == EFS_OK)
            *out = reply;
        return rc;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_lane_bootstrap(efs_export_id_t export_id, efs_ino_t ino, uint64_t gen, uint32_t ci, uint32_t cs,
                                struct efs_msg_lane_writer_view_reply *out)
{
    if (!ino || !gen || !out || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    struct efs_msg_lane_bootstrap req = {export_id, ino, gen, ci, cs};
    uint32_t shard = efs_kv_inode_shard(ino);
    efs_node_id_t target = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = target;
        struct efs_conn *conn = target ? efs_client_conn_get(target) :
                                         rpc_owner_conn_shard(shard, &nid);
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, EFS_MSG_LANE_BOOTSTRAP, &req, sizeof(req))) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        uint8_t type = 0;
        uint32_t len = 0;
        void *payload = NULL;
        if (efs_conn_recv_msg(conn, &type, &payload, &len)) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        if (type != EFS_MSG_LANE_BOOTSTRAP_REPLY || len != sizeof(*out)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        struct efs_msg_lane_writer_view_reply reply;
        memcpy(&reply, payload, sizeof(reply));
        free(payload);
        if (reply.view.status == EFS_INODE_RPC_NOT_PRIMARY && reply.view.primary_id &&
            reply.view.primary_id != nid) {
            target = reply.view.primary_id;
            continue;
        }
        if (reply.view.status == EFS_INODE_RPC_BUSY) {
            if (rpc_writer_retry_pause(attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        int rc = rpc_status_to_efs(reply.view.status);
        if (rc != EFS_OK)
            return rc;
        struct efs_msg_lane_writer_view validation = {ino, gen, ci, cs};
        rc = efs_lane_writer_view_reply_valid(&validation, &reply);
        if (rc == EFS_OK)
            *out = reply;
        return rc;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_writer_admission_view(efs_export_id_t export_id, efs_ino_t ino,
                                          uint64_t gen, uint32_t ci, uint32_t cs,
                                          struct efs_msg_lane_writer_view_reply *out)
{
    if (!ino || !gen || !out || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    struct efs_msg_lane_writer_view_reply view;
    int rc = efs_client_rpc_lane_writer_view(ino, gen, ci, cs, &view);
    /* Only authoritative absence is a bootstrap request. BUSY/STALE/I/O errors
     * never fall back to an inode lookup or mutate authority implicitly. */
    if (rc == EFS_ERR_NOT_FOUND && !efs_client_rpc_past_deadline())
        rc = efs_client_rpc_lane_bootstrap(export_id, ino, gen, ci, cs, &view);
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    if (rc != EFS_OK)
        return rc;
    struct efs_msg_lane_writer_view req = {ino, gen, ci, cs};
    rc = efs_lane_writer_view_reply_valid(&req, &view);
    if (rc == EFS_OK)
        *out = view;
    return rc;
}

int efs_client_rpc_getchunks_fileid(efs_export_id_t export_id, efs_ino_t ino,
                                    uint64_t generation,
                             uint32_t start, struct efs_chunk_rec *recs,
                             uint32_t *inout_count, uint64_t *resolved_generation)
{
    struct efs_msg_inode_getchunks req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.generation = generation;
    req.start = start;
    req.max = inout_count ? *inout_count : EFS_GETCHUNKS_MAX;
    /* Enter at the inode's shard; the host walks the file's lane shards
     * itself and bounces when it does not host them. */
    uint32_t shard = efs_export_shard_of(ino, EFS_KV_SHARD_BITS);
    struct efs_msg_inode_getchunks_reply *r = NULL;
    void *payload = NULL;
    efs_node_id_t target = 0, skip = 0;
    for (int attempt = 0; attempt < 16; attempt++) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = 0;
        int prof = rpc_prof_enabled();
        unsigned long long t0 = prof ? rpc_prof_now_us() : 0;
        struct efs_conn *conn = target ? efs_client_conn_get(target)
                                      : raft_voter_conn_skip(shard, &nid, skip);
        if (target)
            nid = target;
        unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
        if (!conn || efs_conn_send_msg(conn, EFS_MSG_GETCHUNKS_PACKED, &req,
                                      sizeof(req)) != 0) {
            if (conn) efs_client_conn_drop(nid, conn);
            /* This immutable read is safe to repeat after endpoint loss.
             * Drop a dead leader hint and choose a different live voter;
             * preserve FileID, range, outputs and the original deadline. */
            skip = nid; target = 0;
            if (attempt == 15) return EFS_ERR_NET;
            if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        uint8_t rtype = 0;
        uint32_t plen = 0;
        payload = NULL;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            payload = NULL;
            skip = nid; target = 0;
            if (attempt == 15) return EFS_ERR_NET;
            if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        rpc_prof_add(EFS_MSG_INODE_GETCHUNKS, t1 - t0, t2 - t1, t3 - t2, 0);
        if (rtype != EFS_MSG_GETCHUNKS_PACKED_REPLY ||
            plen != sizeof(struct efs_msg_inode_getchunks_reply)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        r = payload;
        if (r->status == EFS_INODE_RPC_NOT_PRIMARY) {
            efs_node_id_t hint = r->primary_id;
            target = hint && hint != nid ? hint : 0;
            skip = nid;
            free(payload);
            payload = NULL;
            r = NULL;
            if (!target && (attempt == 15 ||
                            rpc_writer_retry_pause((unsigned)attempt) != EFS_OK))
                return EFS_ERR_BUSY;
            continue;
        }
        if (r->status != EFS_INODE_RPC_BUSY)
            break;
        free(payload);
        payload = NULL;
        r = NULL;
        if (attempt == 15 || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
            return EFS_ERR_BUSY;
    }
    if (!r) {
        free(payload);
        return EFS_ERR_BUSY;
    }
    if (r->status != EFS_INODE_RPC_OK) {
        int st = rpc_status_to_efs(r->status);
        free(payload);
        return st;
    }
    if (efs_getchunks_reply_valid(&req, r) != EFS_OK) {
        free(payload);
        return EFS_ERR_PROTO;
    }
    uint32_t n = r->count;
    if (inout_count && n > *inout_count)
        n = *inout_count;
    if (recs && n)
        memcpy(recs, r->recs, n * sizeof(recs[0]));
    if (inout_count)
        *inout_count = n;
    if (resolved_generation)
        *resolved_generation = r->generation;
    free(payload);
    return EFS_OK;
}

int efs_client_rpc_getchunks(efs_export_id_t export_id, efs_ino_t ino,
                             uint32_t start, struct efs_chunk_rec *recs,
                             uint32_t *inout_count)
{
    return efs_client_rpc_getchunks_fileid(export_id, ino, 0, start, recs,
                                          inout_count, NULL);
}

int efs_client_rpc_unlink(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, int is_dir)
{
    uint8_t buf[sizeof(struct efs_msg_inode_unlink) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_unlink *req = (struct efs_msg_inode_unlink *)buf;
    struct efs_opid_req q;
    uint32_t slen;
    int slot;

    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->parent = parent;
    if (name)
        strncpy(req->name, name, EFS_MAX_NAME - 1);
    req->is_dir = is_dir ? 1 : 0;
    slot = opid_begin(&q);
    if (slot < 0)
        return EFS_ERR_BUSY;
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(parent, EFS_MSG_INODE_UNLINK, buf, slen,
                                 EFS_MSG_INODE_UNLINK_REPLY, &r, sizeof(r));
    opid_end(slot);
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
}

int efs_client_rpc_rename_at(efs_export_id_t export_id, efs_ino_t old_parent,
                             const char *old_name, efs_ino_t new_parent,
                             const char *new_name, struct efs_inode *out)
{
    uint8_t buf[sizeof(struct efs_msg_inode_rename_at) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_rename_at *req = (struct efs_msg_inode_rename_at *)buf;
    struct efs_opid_req q;
    uint32_t slen;
    int slot;

    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->old_parent = old_parent;
    req->new_parent = new_parent;
    if (old_name)
        strncpy(req->old_name, old_name, EFS_MAX_NAME - 1);
    if (new_name)
        strncpy(req->new_name, new_name, EFS_MAX_NAME - 1);
    slot = opid_begin(&q);
    if (slot < 0)
        return EFS_ERR_BUSY;
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(old_parent, EFS_MSG_INODE_RENAME_AT, buf,
                                 slen, EFS_MSG_INODE_RENAME_AT_REPLY,
                                 &r, sizeof(r));
    opid_end(slot);
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_setattr(efs_export_id_t export_id, efs_ino_t ino,
                           uint32_t mask, uint32_t mode, uid_t uid, gid_t gid,
                           uint64_t size, uint64_t mtime, uint32_t mtime_nsec,
                           uint64_t atime, uint32_t atime_nsec,
                           struct efs_inode *out)
{
    struct efs_msg_inode_setattr req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.mask = mask;
    req.mode = mode;
    req.uid = (uint32_t)uid;
    req.gid = (uint32_t)gid;
    req.size = size;
    req.mtime = mtime;
    req.mtime_nsec = mtime_nsec;
    req.atime = atime;
    req.atime_nsec = atime_nsec;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_SETATTR, &req, sizeof(req),
                                 EFS_MSG_INODE_SETATTR_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

/* Cross-client O_APPEND: reserve the next len bytes at the owner.
 * new_size_out is this reservation's end; start_out is the reserved
 * offset (reply atime) when the host stamped reserved+len. */


int efs_client_rpc_append_reserve(efs_export_id_t export_id, efs_ino_t ino,
                                  uint64_t len, const struct efs_opid_req *q,
                                  uint64_t *new_size_out, uint64_t *start_out)
{
    uint8_t buf[sizeof(struct efs_msg_inode_append) + EFS_APPEND_ACK_LEN];
    struct efs_msg_inode_append *req = (struct efs_msg_inode_append *)buf;
    uint8_t *su = buf + sizeof(*req);
    struct efs_msg_inode_reply r;
    int rc;

    if (!efs_opid_req_valid(q) || q->ack >= q->id.seq)
        return EFS_ERR_INVAL;
    rc=efs_client_session_admit(efs_kv_inode_shard(ino),&q->id);
    if(rc!=EFS_OK)return rc;
    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->ino = ino;
    req->len = len;
    struct efs_opid_req current = *q;
    /* BUSY may have released the per-inode append lock. Refresh only the
     * retirement watermark; preserve this operation's UUID/epoch/sequence. */
    pthread_mutex_lock(&opid_mu);
    uint64_t low = q->id.seq;
    for (int i = 0; i < EFS_OPID_INFLIGHT; i++) {
        uint64_t pending = g_client.opid_inflight[i];
        if (pending && pending < low) low = pending;
    }
    if (low - 1 > current.ack) current.ack = low - 1;
    pthread_mutex_unlock(&opid_mu);
    efs_opid_req_pack(&current, su);
    rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_APPEND, buf, sizeof(buf),
                             EFS_MSG_INODE_APPEND_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (new_size_out)
        *new_size_out = r.inode.size;
    if (start_out)
        *start_out = r.inode.atime;
    return EFS_OK;
}

int efs_client_rpc_hold(efs_export_id_t export_id, efs_ino_t ino, int open,
                        uint64_t owner)
{
    uint8_t buf[sizeof(struct efs_msg_inode_hold)+EFS_SESS_WIRE_LEN];
    struct efs_msg_inode_hold *req=(struct efs_msg_inode_hold *)buf;
    struct efs_opid identity;
    int rc=efs_client_publication_id(1,&identity);
    if (rc!=EFS_OK) return rc;
    if (open) {
        rc=efs_client_rpc_publication_session(efs_kv_inode_shard(ino),&identity);
        if (rc!=EFS_OK) return rc;
    }
    memset(buf,0,sizeof(buf));
    req->export_id=export_id;req->ino=ino;req->flags=open?1u:0u;
    req->owner=owner?owner:g_client.flock_token;
    memcpy(buf+sizeof(*req),identity.client_uuid,EFS_OPID_UUID_LEN);
    memcpy(buf+sizeof(*req)+EFS_OPID_UUID_LEN,&identity.session_epoch,4);
    struct efs_msg_inode_reply reply;
    rc=rpc_send_recv_owner(ino,EFS_MSG_INODE_HOLD,buf,sizeof(buf),
                           EFS_MSG_INODE_HOLD_REPLY,&reply,sizeof(reply));
    return rc==EFS_OK?rpc_status_to_efs(reply.status):rc;
}

int efs_client_rpc_flock(efs_export_id_t export_id, efs_ino_t ino, uint32_t op,
                         uint64_t owner)
{
    return efs_client_rpc_flock_range(export_id, ino, op, owner, 0, 0, NULL);
}

int efs_client_rpc_flock_range(efs_export_id_t export_id, efs_ino_t ino,
                               uint32_t op, uint64_t owner, uint64_t start,
                               uint64_t end, struct efs_msg_inode_reply *reply_out)
{
    uint8_t buf[sizeof(struct efs_msg_inode_flock) + EFS_FLOCK_RANGE_LEN + EFS_SESS_WIRE_LEN];
    struct efs_msg_inode_flock *req = (struct efs_msg_inode_flock *)buf;
    struct efs_msg_inode_reply r;
    uint32_t slen = sizeof(*req);
    int rc;

    struct efs_opid identity;
    rc=efs_client_publication_id(1,&identity);
    if (rc!=EFS_OK) return rc;
    if (!(op & (EFS_FLOCK_UN | EFS_FLOCK_GETLK))) {
        rc=efs_client_rpc_publication_session(efs_kv_inode_shard(ino),&identity);
        if (rc!=EFS_OK) return rc;
    }
    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->ino = ino;
    req->op = op;
    req->owner = owner;
    /* Always send the half-open range. [0,0] is not a lock (host
     * rejects start>=end); treat that as whole-file. Skipping the
     * suffix used to default [0,~0] on the server — same for flock,
     * but a botched FCNTL [0,0] then looked like a whole-file lock. */
    if (start == 0 && end == 0)
        end = ~(uint64_t)0;
    memcpy(buf + sizeof(*req), &start, 8);
    memcpy(buf + sizeof(*req) + 8, &end, 8);
    slen += EFS_FLOCK_RANGE_LEN;
    memcpy(buf+slen,identity.client_uuid,EFS_OPID_UUID_LEN);
    memcpy(buf+slen+EFS_OPID_UUID_LEN,&identity.session_epoch,4);
    slen += EFS_SESS_WIRE_LEN;
    rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_FLOCK, buf, slen,
                             EFS_MSG_INODE_FLOCK_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (reply_out)
        *reply_out = r;
    return rpc_status_to_efs(r.status);
}

int efs_client_rpc_link(efs_export_id_t export_id, efs_ino_t src_ino,
                        efs_ino_t new_parent, const char *new_name,
                        struct efs_inode *out)
{
    uint8_t buf[sizeof(struct efs_msg_inode_link) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_link *req = (struct efs_msg_inode_link *)buf;
    struct efs_opid_req q;
    uint32_t slen;
    int slot;

    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->src_ino = src_ino;
    req->new_parent = new_parent;
    if (new_name)
        strncpy(req->new_name, new_name, EFS_MAX_NAME - 1);
    slot = opid_begin(&q);
    if (slot < 0)
        return EFS_ERR_BUSY;
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(new_parent, EFS_MSG_INODE_LINK, buf, slen,
                                 EFS_MSG_INODE_LINK_REPLY, &r, sizeof(r));
    opid_end(slot);
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}


void efs_client_rpc_set_deadline_ms(uint64_t mono_ms)
{
    efs_net_set_deadline_ms(mono_ms);
}

uint64_t efs_client_rpc_deadline_ms(void)
{
    return efs_net_deadline_ms();
}

static int rpc_past_deadline(void)
{
    struct timespec ts;
    uint64_t now;

    uint64_t deadline = efs_net_deadline_ms();
    if (!deadline)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    return now >= deadline;
}

int efs_client_rpc_past_deadline(void)
{
    return rpc_past_deadline();
}

/* rpc_send_recv_shard with the dual-host picker (raft mode reports). Same
 * NOT_PRIMARY hint-following; BUSY backs off and retries. recv_ms > 0
 * bounds the reply wait for this call instead of the conn's 30 s
 * default: a REPORT's service time grows with its record count, and a
 * recv timeout on a request the server is still executing re-sends the
 * whole request (Oct 1 2026: 8 × 20 GiB close REPORTs, attempts=6,
 * 116 s). */
static int rpc_send_recv_dual(uint8_t type, const void *req, uint32_t req_len,
                              uint8_t expect, void *reply, uint32_t reply_len,
                              int recv_ms)
{
    efs_node_id_t target = 0;
    efs_node_id_t skip = 0;
    int saw_busy = 0;
    int prof = rpc_prof_enabled();
    unsigned long long t_start = rpc_prof_now_us();
    union { struct efs_msg_inode_create create; struct efs_msg_inode_unlink unlink;
            struct efs_msg_inode_link link; struct efs_msg_inode_rename_at rename; } mutation;
    uint8_t create_retry[sizeof(mutation)+EFS_OPID_WIRE_LEN];
    int identified_create=(type==EFS_MSG_INODE_CREATE && req_len==sizeof(mutation.create)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_UNLINK && req_len==sizeof(mutation.unlink)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_LINK && req_len==sizeof(mutation.link)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_RENAME_AT && req_len==sizeof(mutation.rename)+EFS_OPID_WIRE_LEN);
    if (identified_create) { memcpy(create_retry,req,req_len);req=create_retry; }
    /* MKDIR owns a bounded callback deadline. Keep its original identity
     * while a cross-shard prepare drains instead of stopping after 16 tries.
     * Other users retain the legacy attempt cap. */
    int attempts = identified_create && type == EFS_MSG_INODE_CREATE &&
        S_ISDIR(((const struct efs_msg_inode_create *)req)->mode) &&
        efs_client_rpc_deadline_ms() ? 64 : 16;
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (identified_create) opid_retry_ack(create_retry,req_len);
        efs_node_id_t nid = 0;
        struct efs_conn *conn;
        unsigned long long t0;
        if (rpc_past_deadline())
            return EFS_ERR_BUSY;
        t0 = prof ? rpc_prof_now_us() : 0;
        if (target != 0) {
            conn = efs_client_conn_get(target);
            nid = target;
        } else {
            conn = raft_dual_voter_conn(&nid, skip);
        }
        unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
        if (!conn || efs_conn_send_msg(conn, type, req, req_len) != 0) {
            if (conn)
                efs_client_conn_drop(nid, conn);
            if (attempt + 1 < attempts) {
                if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                    return EFS_ERR_BUSY;
                continue;
            }
            return EFS_ERR_NET;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        uint8_t rtype = 0;
        void *payload = NULL;
        uint32_t plen = 0;
        if (recv_ms > 0)
            efs_conn_set_recv_timeout(conn, recv_ms);
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            if (attempt + 1 < attempts) {
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=recv rc=%d "
                        "req_len=%u recv_ms=%d\n",
                        type, attempt, rc, req_len,
                        recv_ms > 0 ? recv_ms : EFS_IO_TIMEOUT_MS);
                if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                    return EFS_ERR_BUSY;
                continue;
            }
            return EFS_ERR_NET;
        }
        if (recv_ms > 0)
            efs_conn_set_recv_timeout(conn, 0); /* pooled conn: default again */
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        if (rtype != expect || plen < reply_len) {
            fprintf(stderr, "inode-rpc: proto type=%u rtype=%u plen=%u "
                    "reply_len=%u nid=%u\n",
                    type, rtype, plen, reply_len, nid);
            free(payload);
            return EFS_ERR_PROTO;
        }
        memcpy(reply, payload, reply_len);
        free(payload);
        struct efs_msg_inode_reply *r = reply;
        if (r->status == EFS_INODE_RPC_BUSY) {
            saw_busy = 1;
            skip = nid;
            target = 0;
            unsigned long long sleep_start = rpc_prof_now_us();
            if (attempt + 1 == attempts || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        if (r->status == EFS_INODE_RPC_STALE && stale_retryable(type)) {
            /* PREPARE can report the participant leader whose canonical
             * version disagreed with this replica. Re-reading repeatedly on
             * the same coordinator cannot resolve that replica-local drift. */
            target = r->primary_id && r->primary_id <= EFS_MAX_NODES &&
                     r->primary_id != nid ? r->primary_id : 0;
            skip = target ? 0 : nid;
            /* See rpc_send_recv_shard: a directory-op txn that lost a
             * dentry / witness race committed nothing; re-run it.
             * REPORT_CHUNKS owns its STALE (W1). */
            saw_busy = 1;
            unsigned long long sleep_start = rpc_prof_now_us();
            if (attempt + 1 == attempts || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        if (r->status != EFS_INODE_RPC_NOT_PRIMARY) {
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
            rpc_note_slow_ok(type, attempt, saw_busy, r->status, t_start);
            return EFS_OK;
        }
        if (r->primary_id == 0 || r->primary_id == nid) {
            /* Hintless NOT_PRIMARY = election in progress; transient,
             * back off like BUSY (see rpc_send_recv_shard). */
            unsigned long long sleep_start = rpc_prof_now_us();
            saw_busy = 1;
            target = 0;
            if (attempt + 1 == attempts || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
        target = r->primary_id;
    }
    if (saw_busy)
        fprintf(stderr, "inode-rpc: dual type=%u exhausted %d BUSY/STALE "
                "retries (%.1f s) -> EBUSY\n", type, attempts, (rpc_prof_now_us() - t_start) / 1000000.0);
    return saw_busy ? EFS_ERR_BUSY : EFS_ERR_NOT_PRIMARY;
}

int efs_client_rpc_report_dirty_raft(efs_export_id_t export_id,
                                     const struct efs_chunk_rec *recs,
                                     uint32_t count,
                                     const struct efs_ino_size_rec *irecs,
                                     uint32_t ino_count, int sync)
{
    if (count == 0 && ino_count == 0 && !sync)
        return EFS_OK;
    if ((count && !recs) || (ino_count && !irecs))
        return EFS_ERR_INVAL;
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    size_t limit = EFS_MSG_MAX_LEN - 1u;
    size_t len = sizeof(struct efs_msg_report_chunks) + EFS_REPORT_SESSION_LEN;
    /* Reject impossible counts before touching arrays or allocating. Keep
     * every subtraction in size_t and the eventual wire cast representable. */
    if (ino_count > (limit - len) / sizeof(struct efs_ino_size_rec))
        return EFS_ERR_INVAL;
    len += (size_t)ino_count * sizeof(struct efs_ino_size_rec);
    if (count > (limit - len) / offsetof(struct efs_chunk_rec, deltas))
        return EFS_ERR_INVAL;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (!(i & 255u) && efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        if (recs[i].delta_base_n > EFS_CHUNK_DELTA_MAX)
            return EFS_ERR_INVAL;
        size_t bytes = efs_chunk_rec_wire_size(&recs[i]);
        if (bytes > limit - len)
            return EFS_ERR_INVAL;
        len += bytes;
    }
    struct efs_opid session;
    /* Query the shared mount identity; this does not allocate/ACK an operation. */
    int identity_rc = efs_client_publication_id(1, &session);
    if (identity_rc != EFS_OK) return identity_rc;
    if (!efs_report_session_valid(&session)) return EFS_ERR_INVAL;
    uint8_t *buf = malloc(len);
    if (!buf)
        return EFS_ERR_NOMEM;
    struct efs_msg_report_chunks *hdr = (struct efs_msg_report_chunks *)buf;
    hdr->export_id = export_id;
    hdr->count = count;
    hdr->sync = (sync ? EFS_REPORT_F_SYNC : 0u) | EFS_REPORT_F_SESSION;
    hdr->ino_count = ino_count;
    uint8_t *p = buf + sizeof(*hdr);
    if (count) {
        for (i = 0; i < count; i++) {
            size_t n = efs_chunk_rec_pack(p, len - (size_t)(p - buf), &recs[i]);
            if (n == 0) {
                free(buf);
                return EFS_ERR_NOMEM;
            }
            p += n;
        }
    }
    if (ino_count)
        memcpy(p, irecs, (size_t)ino_count * sizeof(*irecs));
    p += (size_t)ino_count * sizeof(*irecs);
    efs_report_session_pack(p, &session);
    struct efs_msg_inode_reply r;
    /* Reply wait sized to the work: the server packs, publishes and
     * waits for a Raft commit per record (13 us/rec alone, 8 concurrent
     * 160k-rec REPORTs went past 30 s). 0.5 ms per record on top of the
     * default; D24's periodic REPORT keeps a sequential writer's close
     * near 8192 records, so this is the bound for the rest. */
    int recv_ms = EFS_IO_TIMEOUT_MS + (int)(count / 2u);
    int rc = rpc_send_recv_dual(EFS_MSG_REPORT_CHUNKS, buf, (uint32_t)len,
                                EFS_MSG_REPORT_CHUNKS_REPLY, &r, sizeof(r),
                                recv_ms);
    free(buf);
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
}

int efs_client_rpc_xattr(efs_export_id_t export_id, efs_ino_t ino, uint8_t op,
                         uint32_t flags, const void *name, uint16_t nlen,
                         const void *val, uint32_t vlen, void *out,
                         uint32_t *out_len)
{
    uint8_t reqbuf[sizeof(struct efs_msg_xattr) + EFS_XATTR_NAME_MAX +
                   EFS_XATTR_VALUE_MAX];
    struct efs_msg_xattr_reply rep;
    struct efs_msg_xattr *req;
    uint32_t req_len, got = 0, hdr;
    int rc;

    if (nlen > EFS_XATTR_NAME_MAX || vlen > EFS_XATTR_VALUE_MAX)
        return EFS_ERR_INVAL;
    if ((nlen && !name) || (vlen && !val))
        return EFS_ERR_INVAL;
    req = (struct efs_msg_xattr *)reqbuf;
    memset(req, 0, sizeof(*req));
    req->export_id = export_id;
    req->flags = flags;
    req->ino = ino;
    req->vlen = vlen;
    req->nlen = nlen;
    req->op = op;
    if (nlen)
        memcpy(reqbuf + sizeof(*req), name, nlen);
    if (vlen)
        memcpy(reqbuf + sizeof(*req) + nlen, val, vlen);
    req_len = (uint32_t)sizeof(*req) + (uint32_t)nlen + vlen;
    hdr = (uint32_t)offsetof(struct efs_msg_xattr_reply, data);
    memset(&rep, 0, hdr);
    rc = rpc_send_recv_shard(efs_export_shard_of(ino, EFS_KV_SHARD_BITS),
                             EFS_MSG_XATTR, reqbuf, req_len,
                             EFS_MSG_XATTR_REPLY, &rep, hdr, sizeof(rep), &got);
    if (rc != EFS_OK)
        return rc;
    if (rep.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(rep.status);
    if (got < hdr || rep.nbytes > got - hdr || rep.nbytes > EFS_XATTR_BLOB_MAX)
        return EFS_ERR_PROTO;
    if (out_len) {
        if (out && *out_len < rep.nbytes) {
            *out_len = rep.nbytes;
            return EFS_ERR_INVAL;
        }
        if (out && rep.nbytes)
            memcpy(out, rep.data, rep.nbytes);
        *out_len = rep.nbytes;
    }
    return EFS_OK;
}

static int query_snapshot_rpc(const struct efs_msg_query_snapshot *req,uint32_t req_len,
                                struct efs_msg_query_snapshot_reply *reply,char **text,efs_node_id_t preferred)
{
    if (!req || !reply || !text) return EFS_ERR_INVAL;
    *text=NULL;memset(reply,0,sizeof(*reply));
    efs_node_id_t target=preferred,skip=0,pinned=0;
    if (req->version==EFS_QUERY_PAGE_VERSION) {
        uint64_t token=((const struct efs_msg_query_snapshot_v4 *)req)->token;
        if (token) target=pinned=(efs_node_id_t)(token>>56);
    }
    int last=EFS_ERR_BUSY;
    for (unsigned attempt=0;attempt<16;attempt++) {
        if (efs_client_rpc_past_deadline()) return EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target ? efs_client_conn_get(target) : raft_dual_voter_conn(&nid,skip);
        if (!conn) {
            last=EFS_ERR_NET;
            if (pinned) return last;
            skip=nid;target=0;
        }
        else {
            uint8_t type=0;void *payload=NULL;uint32_t len=0;
            if (efs_conn_send_msg(conn,EFS_MSG_QUERY_SNAPSHOT,req,req_len) ||
                efs_conn_recv_msg(conn,&type,&payload,&len)) {
                efs_client_conn_drop(nid,conn);free(payload);last=EFS_ERR_NET;
                if (pinned) return last;
                skip=nid;target=0;
            } else {
                efs_client_conn_release(nid,conn);
                if (type!=EFS_MSG_QUERY_SNAPSHOT_REPLY || len<sizeof(*reply)) { free(payload);return EFS_ERR_PROTO; }
                memcpy(reply,payload,sizeof(*reply));
                if (reply->version!=req->version || reply->text_len>req->max_output ||
                    len!=sizeof(*reply)+reply->text_len || (reply->status && reply->text_len)) {
                    free(payload);return EFS_ERR_PROTO;
                }
                last=reply->status;
                if (!last) {
                    /* Local routing metadata: repeated unpinned FUSE callbacks
                     * must return to the publisher that created this page. */
                    reply->primary_id=nid;
                    *text=malloc((size_t)reply->text_len+1);
                    if (!*text) { free(payload);return EFS_ERR_NOMEM; }
                    if (memchr((uint8_t *)payload+sizeof(*reply),0,reply->text_len)) {
                        free(*text);*text=NULL;free(payload);return EFS_ERR_PROTO;
                    }
                    memcpy(*text,(uint8_t *)payload+sizeof(*reply),reply->text_len);
                    (*text)[reply->text_len]=0;free(payload);return EFS_OK;
                }
                free(payload);
                if (last!=EFS_ERR_BUSY && last!=EFS_ERR_NOT_PRIMARY && last!=EFS_ERR_NO_QUORUM) return last;
                if (pinned) return last;
                target=last==EFS_ERR_NOT_PRIMARY && reply->primary_id<=EFS_MAX_NODES ? reply->primary_id : 0;
                skip=target ? 0 : nid;
            }
        }
        if (attempt+1<16 && rpc_writer_retry_pause(attempt)!=EFS_OK) return EFS_ERR_BUSY;
    }
    return last;
}

int efs_client_rpc_query_snapshot(const struct efs_msg_query_snapshot *req,
                                    struct efs_msg_query_snapshot_reply *reply,char **text)
{
    if (req && req->version==EFS_QUERY_FILTER_VERSION) return EFS_ERR_INVAL;
    return query_snapshot_rpc(req,sizeof(*req),reply,text,0);
}
int efs_client_rpc_query_snapshot_v4(const struct efs_msg_query_snapshot_v4 *req,
                                    struct efs_msg_query_snapshot_reply *reply,char **text)
{
    if (!req) return EFS_ERR_INVAL;
    return query_snapshot_rpc(&req->base,req->base.version==EFS_QUERY_PAGE_VERSION ? sizeof(*req) :
        req->base.version==EFS_QUERY_FILTER_VERSION ? sizeof(struct efs_msg_query_snapshot_v3) : sizeof(req->base),reply,text,0);
}

/* Preferred owner is a recoverable first-page hint; token ownership remains strict. */
int efs_client_rpc_query_snapshot_v4_at(const struct efs_msg_query_snapshot_v4 *req,
                                       efs_node_id_t preferred,
                                       struct efs_msg_query_snapshot_reply *reply,char **text)
{
    if (!req || preferred>EFS_MAX_NODES) return EFS_ERR_INVAL;
    return query_snapshot_rpc(&req->base,req->base.version==EFS_QUERY_PAGE_VERSION ? sizeof(*req) :
        req->base.version==EFS_QUERY_FILTER_VERSION ? sizeof(struct efs_msg_query_snapshot_v3) : sizeof(req->base),reply,text,preferred);
}
