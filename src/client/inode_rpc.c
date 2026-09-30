#include "client_internal.h"
#include "efs/opid.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
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
    case EFS_INODE_RPC_BUSY:        return EFS_ERR_BUSY;
    case EFS_INODE_RPC_INVAL:       return EFS_ERR_INVAL;
    case EFS_INODE_RPC_NOT_PRIMARY: return EFS_ERR_NOT_PRIMARY;
    case EFS_INODE_RPC_NOT_EMPTY:   return EFS_ERR_NOT_EMPTY;
    case EFS_INODE_RPC_STALE:       return EFS_ERR_STALE;
    case EFS_INODE_RPC_NODATA:      return EFS_ERR_NODATA;
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
 * BUSY / STALE / NOT_PRIMARY retry inside rpc_send_recv_* reuses the
 * same bytes, which is the whole point. Table full = the op goes out
 * without an identity (unprotected, as before I16). */
static pthread_mutex_t opid_mu = PTHREAD_MUTEX_INITIALIZER;

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

/* Fill q and take a slot (returned; -1 = none, q left invalid). */
static int opid_begin(struct efs_opid_req *q)
{
    int i, slot = -1;
    uint64_t low = 0;

    memset(q, 0, sizeof(*q));
    pthread_mutex_lock(&opid_mu);
    opid_seed_locked();
    for (i = 0; i < EFS_OPID_INFLIGHT; i++) {
        uint64_t s = g_client.opid_inflight[i];

        if (s == 0) {
            if (slot < 0)
                slot = i;
        } else if (low == 0 || s < low) {
            low = s;
        }
    }
    if (slot >= 0) {
        uint64_t seq = g_client.opid_next++;

        g_client.opid_inflight[slot] = seq;
        memcpy(q->id.client_uuid, g_client.opid_uuid, EFS_OPID_UUID_LEN);
        q->id.session_epoch = g_client.opid_epoch;
        q->id.seq = seq;
        q->ack = (low ? low : seq) - 1;
    }
    pthread_mutex_unlock(&opid_mu);
    return slot;
}

static void opid_end(int slot)
{
    if (slot < 0)
        return;
    pthread_mutex_lock(&opid_mu);
    g_client.opid_inflight[slot] = 0;
    pthread_mutex_unlock(&opid_mu);
}

/* Append the wire suffix after `slen` bytes of struct; new length. */
static uint32_t opid_suffix(uint8_t *buf, uint32_t slen, const struct efs_opid_req *q)
{
    if (!efs_opid_req_valid(q))
        return slen;
    efs_opid_req_pack(q, buf + slen);
    return slen + EFS_OPID_WIRE_LEN;
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

/* First live voter of the shard's group, not `skip`; the host bounces to
 * the leader and the retry loop follows primary_id, so any voter is a
 * fine entry point. `skip` is the voter that just answered NOT_PRIMARY
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
    int pass;

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
    struct efs_conn *held = NULL;
    efs_node_id_t held_id = 0;

    for (int rid = 0; rid < n && rid < EFS_RAFT_MAX_PEERS; rid++) {
        if (!(dual & (1u << rid)))
            continue;
        efs_node_id_t id = (efs_node_id_t)(rid + 1);
        if (efs_client_node_is_down(id))
            continue;
        struct efs_conn *conn = efs_client_conn_get(id);
        if (!conn)
            continue;
        /* A report that just got BUSY from this node tries the other
         * dual-host. Both host every group; the first one (fcstor004)
         * was the only target, and a follower that has fallen behind
         * turns every ReadIndex into BUSY while its peer is caught up. */
        if (skip && id == skip) {
            if (!held) {
                held = conn;
                held_id = id;
            } else {
                efs_client_conn_release(id, conn);
            }
            continue;
        }
        if (held)
            efs_client_conn_release(held_id, held);
        *nid_out = id;
        return conn;
    }
    if (held) {
        *nid_out = held_id;
        return held;
    }
    return raft_voter_conn(EFS_ROOT_INO, nid_out);
}

static struct efs_conn *rpc_owner_conn_shard(uint32_t shard,
                                             efs_node_id_t *nid_out)
{
    return raft_voter_conn(shard, nid_out);
}

static int rpc_send_recv_dual(uint8_t type, const void *req, uint32_t req_len,
                              uint8_t expect, void *reply, uint32_t reply_len);

/* Send to the owner of `shard`. Retry NOT_PRIMARY. */
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
    for (int attempt = 0; attempt < 16; attempt++) {
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
            /* One dropped conn is not a failed create. Same 16-attempt
             * budget as BUSY; a hard NET only after it is used up
             * (dir_many_files EIO on a single recv failure). */
            if (attempt + 1 < 16) {
                unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=send\n",
                        type, attempt);
                usleep((useconds_t)(50000ull << shift));
                continue;
            }
            return EFS_ERR_NET;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        uint8_t rtype = 0;
        void *payload = NULL;
        uint32_t plen = 0;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            efs_client_conn_drop(nid, conn);
            if (attempt + 1 < 16) {
                unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=recv rc=%d\n",
                        type, attempt, rc);
                usleep((useconds_t)(50000ull << shift));
                continue;
            }
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
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
        if (r->status == EFS_INODE_RPC_BUSY) {
            saw_busy = 1;
            /* INODE_APPEND uses BUSY as the unflushed-reservation barrier.
             * The caller must flush + report before retrying; spinning here
             * holds g_append_mu for ~10s and deadlocks concurrent O_APPEND. */
            if (type == EFS_MSG_INODE_APPEND || type == EFS_MSG_INODE_FLOCK) {
                rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
                return EFS_OK;
            }
            /* Extra-shard owner is still assembling pages after restart.
             * Same target — do not flip to another node. */
            unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
            unsigned long long sleep_us = 50000ull << shift;
            usleep((useconds_t)sleep_us);
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        if (r->status == EFS_INODE_RPC_STALE && stale_retryable(type)) {
            /* A directory-op txn lands STALE when a same-name dentry, the
             * child row, or an emptiness witness moved between its read
             * and its PREPARE (the parent row itself is a commutative
             * reduce since §7.2 and never conflicts). Nothing committed;
             * re-running re-evaluates against the new state (EEXIST /
             * ENOENT / ENOTEMPTY as appropriate), so retry like BUSY.
             * REPORT_CHUNKS owns its STALE (W1 refetch+overlay) — never
             * retried here. */
            saw_busy = 1;
            unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
            unsigned long long sleep_us = 50000ull << shift;
            usleep((useconds_t)sleep_us);
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
            unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
            unsigned long long sleep_us = 50000ull << shift;
            saw_busy = 1;
            target = 0;
            skip = nid;
            fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=no-hint from=%u\n",
                    type, attempt, nid);
            usleep((useconds_t)sleep_us);
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
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    /* Files co-locate with the parent (one group). MKDIR scatters the
     * child inode, so a parent-group-only voter has to bounce the whole
     * RPC to a dual-host — an extra RTT on ~half of mkdirs. Send dirs
     * to a dual-host so both groups are local. */
    int rc = S_ISDIR(mode)
                 ? rpc_send_recv_dual(EFS_MSG_INODE_CREATE, buf, slen,
                                      EFS_MSG_INODE_CREATE_REPLY, &r,
                                      sizeof(r))
                 : rpc_send_recv_owner(parent, EFS_MSG_INODE_CREATE, buf,
                                       slen, EFS_MSG_INODE_CREATE_REPLY,
                                       &r, sizeof(r));
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

    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    req.max_ents = inout_count ? *inout_count : EFS_READDIR_MAX;
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
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    efs_client_conn_release(nid, conn);
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

int efs_client_rpc_getchunks(efs_export_id_t export_id, efs_ino_t ino,
                             uint32_t start, struct efs_chunk_rec *recs,
                             uint32_t *inout_count)
{
    struct efs_msg_inode_getchunks req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.start = start;
    req.max = inout_count ? *inout_count : EFS_GETCHUNKS_MAX;
    /* Enter at the inode's shard; the host walks the file's lane shards
     * itself and bounces when it does not host them. */
    uint32_t shard = efs_export_shard_of(ino, EFS_KV_SHARD_BITS);
    struct efs_msg_inode_getchunks_reply *r = NULL;
    void *payload = NULL;
    efs_node_id_t target = 0;
    for (int attempt = 0; attempt < 16; attempt++) {
        efs_node_id_t nid = 0;
        int prof = rpc_prof_enabled();
        unsigned long long t0 = prof ? rpc_prof_now_us() : 0;
        struct efs_conn *conn = target ? efs_client_conn_get(target)
                                      : rpc_owner_conn_shard(shard, &nid);
        if (target)
            nid = target;
        unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, EFS_MSG_INODE_GETCHUNKS, &req,
                              sizeof(req)) != 0) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        uint8_t rtype = 0;
        uint32_t plen = 0;
        payload = NULL;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        rpc_prof_add(EFS_MSG_INODE_GETCHUNKS, t1 - t0, t2 - t1, t3 - t2, 0);
        if (rtype != EFS_MSG_INODE_GETCHUNKS_REPLY ||
            plen < sizeof(struct efs_msg_inode_getchunks_reply)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        r = payload;
        if (r->status == EFS_INODE_RPC_NOT_PRIMARY && r->primary_id &&
            r->primary_id != nid) {
            target = r->primary_id;
            free(payload);
            payload = NULL;
            r = NULL;
            continue;
        }
        if (r->status != EFS_INODE_RPC_BUSY)
            break;
        free(payload);
        payload = NULL;
        r = NULL;
        unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
        usleep(50000u << shift);
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
    uint32_t n = r->count;
    if (inout_count && n > *inout_count)
        n = *inout_count;
    if (recs && n)
        memcpy(recs, r->recs, n * sizeof(recs[0]));
    if (inout_count)
        *inout_count = n;
    free(payload);
    return EFS_OK;
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
                                  uint64_t len, uint64_t seq,
                                  uint64_t *new_size_out, uint64_t *start_out)
{
    uint8_t buf[sizeof(struct efs_msg_inode_append) + EFS_APPEND_OPID_LEN];
    struct efs_msg_inode_append *req = (struct efs_msg_inode_append *)buf;
    uint8_t *su = buf + sizeof(*req);
    uint32_t epoch = 1;
    struct efs_msg_inode_reply r;
    int rc;

    if (!g_client.flock_token) {
        g_client.flock_token = ((uint64_t)getpid() << 1) ^ 1ull;
        if (!g_client.flock_token)
            g_client.flock_token = 1;
    }
    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->ino = ino;
    req->len = len;
    memcpy(su, &g_client.flock_token, sizeof(g_client.flock_token));
    memcpy(su + EFS_OPID_UUID_LEN, &epoch, 4);
    memcpy(su + EFS_SESS_WIRE_LEN, &seq, 8);
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
    struct efs_msg_inode_hold req;
    memset(&req, 0, sizeof(req));
    if (!g_client.flock_token) {
        g_client.flock_token = ((uint64_t)getpid() << 1) ^ 1ull;
        if (!g_client.flock_token)
            g_client.flock_token = 1;
    }
    if (!owner)
        owner = g_client.flock_token;
    req.export_id = export_id;
    req.ino = ino;
    req.flags = open ? 1u : 0u;
    req.owner = owner;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_HOLD, &req, sizeof(req),
                                 EFS_MSG_INODE_HOLD_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
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
    uint8_t buf[sizeof(struct efs_msg_inode_flock) + EFS_FLOCK_RANGE_LEN];
    struct efs_msg_inode_flock *req = (struct efs_msg_inode_flock *)buf;
    struct efs_msg_inode_reply r;
    uint32_t slen = sizeof(*req);
    int rc;

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

static __thread uint64_t tl_rpc_deadline_ms;

void efs_client_rpc_set_deadline_ms(uint64_t mono_ms)
{
    tl_rpc_deadline_ms = mono_ms;
}

uint64_t efs_client_rpc_deadline_ms(void)
{
    return tl_rpc_deadline_ms;
}

static int rpc_past_deadline(void)
{
    struct timespec ts;
    uint64_t now;

    if (!tl_rpc_deadline_ms)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    return now >= tl_rpc_deadline_ms;
}

int efs_client_rpc_past_deadline(void)
{
    return rpc_past_deadline();
}

/* rpc_send_recv_shard with the dual-host picker (raft mode reports). Same
 * NOT_PRIMARY hint-following; BUSY backs off and retries. */
static int rpc_send_recv_dual(uint8_t type, const void *req, uint32_t req_len,
                              uint8_t expect, void *reply, uint32_t reply_len)
{
    efs_node_id_t target = 0;
    efs_node_id_t skip = 0;
    int saw_busy = 0;
    int prof = rpc_prof_enabled();
    unsigned long long t_start = rpc_prof_now_us();
    for (int attempt = 0; attempt < 16; attempt++) {
        efs_node_id_t nid = 0;
        struct efs_conn *conn;
        unsigned long long t0;
        if (rpc_past_deadline())
            return saw_busy ? EFS_ERR_BUSY : EFS_ERR_IO;
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
            if (attempt + 1 < 16) {
                unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
                usleep((useconds_t)(50000ull << shift));
                continue;
            }
            return EFS_ERR_NET;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        uint8_t rtype = 0;
        void *payload = NULL;
        uint32_t plen = 0;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            efs_client_conn_drop(nid, conn);
            if (attempt + 1 < 16) {
                unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=recv rc=%d\n",
                        type, attempt, rc);
                usleep((useconds_t)(50000ull << shift));
                continue;
            }
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
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
            unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
            unsigned long long sleep_us = 50000ull << shift;
            usleep((useconds_t)sleep_us);
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        if (r->status == EFS_INODE_RPC_STALE && stale_retryable(type)) {
            /* See rpc_send_recv_shard: a directory-op txn that lost a
             * dentry / witness race committed nothing; re-run it.
             * REPORT_CHUNKS owns its STALE (W1). */
            saw_busy = 1;
            unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
            unsigned long long sleep_us = 50000ull << shift;
            usleep((useconds_t)sleep_us);
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
            unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
            unsigned long long sleep_us = 50000ull << shift;
            saw_busy = 1;
            target = 0;
            usleep((useconds_t)sleep_us);
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
        target = r->primary_id;
    }
    if (saw_busy)
        fprintf(stderr, "inode-rpc: dual type=%u exhausted 16 BUSY/STALE "
                "retries (%.1f s) -> EBUSY\n", type, 10.35);
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
    size_t len = sizeof(struct efs_msg_report_chunks) +
                 (size_t)count * sizeof(struct efs_chunk_rec) +
                 (size_t)ino_count * sizeof(struct efs_ino_size_rec);
    uint8_t *buf = malloc(len);
    if (!buf)
        return EFS_ERR_NOMEM;
    struct efs_msg_report_chunks *hdr = (struct efs_msg_report_chunks *)buf;
    hdr->export_id = export_id;
    hdr->count = count;
    hdr->sync = sync ? 1u : 0u;
    hdr->ino_count = ino_count;
    uint8_t *p = buf + sizeof(*hdr);
    if (count) {
        memcpy(p, recs, (size_t)count * sizeof(*recs));
        p += (size_t)count * sizeof(*recs);
    }
    if (ino_count)
        memcpy(p, irecs, (size_t)ino_count * sizeof(*irecs));
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_dual(EFS_MSG_REPORT_CHUNKS, buf, (uint32_t)len,
                                EFS_MSG_REPORT_CHUNKS_REPLY, &r, sizeof(r));
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
