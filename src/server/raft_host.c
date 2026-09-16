#include "server_internal.h"
#include "efs/checksum.h"
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/dir_layout.h"
#include "efs/dir_spread.h"
#include "efs/opid.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include "efs/session.h"
#include "efs/lock.h"
#include "efs/metadata.h"
#include "efs/store.h"
#include "efs/wire.h"
#include "efs/network.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HOST_TICK_US       5000
#define HOST_HB_TICKS      10
/* Uniform base for all voters: de-synchronization comes from the seeded
 * randomized deadline in raft.c (drawn per election from [base, 2*base)),
 * not a per-node stagger. A fixed stagger lets a behind-log short-timeout
 * node livelock an up-to-date long-timeout one by resetting its timer via
 * maybe_step_down faster than the longer timer can ever fire. */
#define HOST_ELECT_BASE    100
/* Per-peer sender threads do the blocking send+ACK for cfg.send messages.
 * A dead peer must not sit on the 30s pool SO_RCVTIMEO or its outbox
 * becomes a 30s/message drip. Restore the pool timeout before release so
 * bounce RPCs keep the long budget. */
#define HOST_SEND_IO_MS    250
#define HOST_INBOX_MAX     256
/* Per-peer outbound queue cap. Full -> drop newest: a dropped packet is
 * indistinguishable from loss, which Raft retries through. Bumped 256 ->
 * 2048: the synchronous sender drains at the raft-message RTT (~1-4 ms),
 * so a slow peer backs the queue up and 256 dropped under the steady
 * heartbeat/commit/ReadIndex broadcast (observed hi=256 + drops, which
 * put fcstor005 thousands of entries behind). Batched catch-up
 * (EFS_RAFT_AE_MAX) is the real fix; this is headroom so a transient slow
 * phase doesn't drop. */
#define HOST_OUTBOX_MAX    2048
#define HOST_ENCODE_STACK  (64 * 1024)
#define HOST_NGROUPS       2
#define HOST_READ_TRIES    80 /* 80 × 5 ms = 400 ms; heartbeat is 50 ms */
#define HOST_CREATE_NAME_OFF 31
#define HOST_CMD_MAX       512
#define HOST_SETATTR_LEN   61
#define HOST_UTIMENS_LEN   73
/* TRUNCATE: +8 lane_mask (which active lanes THIS entry fences — the ones
 * whose shard maps to the inode's group) +1 flags (bit0: tail_external —
 * the tail chunk's lane is on another group and is published separately
 * after commit). Cross-group lanes were fenced by LANE_FENCE entries on
 * their own groups before this entry was proposed. */
#define HOST_TRUNC_LEN     63
#define HOST_TRUNC_F_TAIL_EXT 1
#define HOST_APPEND_RSV_LEN 45
#define HOST_APPEND_RES_LEN 38
#define HOST_TRUNC_TAIL    (4u + 8u + 8u + 4u + \
                            (uint32_t)EFS_NUM_FRAGMENTS * (4u + EFS_HASH_SIZE))
/* PUBLISH: +8 inode_gen +8 mtime_gen +1 flags (bit0: lane_local — applied
 * on the lane's group, whose KV has no inode row; see meta_apply.h). */
#define HOST_PUBLISH_LEN   (29u + (uint32_t)EFS_NUM_FRAGMENTS * (4u + EFS_HASH_SIZE) + \
                            EFS_OPID_UUID_LEN + 4u + 8u + 8u + 8u + 4u + \
                            8u + 8u + 1u)
#define HOST_PUB_F_LANE_LOCAL 1
#define HOST_PUB_TAIL_TRIES  4 /* cross-group truncate tail CAS retries */
#define HOST_ACTIVATE_LANE_LEN 10 /* tag + ino:8 + lane:1 */
#define HOST_LANE_FENCE_LEN    39 /* tag + ino:8 + gen:8 + lane:1 +
                                   * epoch:8 + size:8 + tail_ci:4 +
                                   * has_tail:1 — matches meta_cmd.h */
#define HOST_DIR_LEN       10
#define HOST_SPREAD_MAX    8 /* leftovers per GC tick; batch, not a scan */
#define HOST_SESS_HDR_LEN  18 /* tag+sub+uuid; same as sim pack_hdr */
#define HOST_SESS_CREATE_LEN 22 /* hdr+epoch */
#define HOST_SESS_REG_LEN  26 /* hdr+epoch+shard */
#define HOST_SESS_GET      0  /* not a log cmd: ReadIndex + efs_session_get */
#define HOST_SESS_LEASE_LEN 38 /* tag+sub+uuid+epoch+ino+gen */
#define HOST_LOCK_LEN 65 /* tag+kind+ino+gen+dom+type+range+owner; sim pack_lock */

struct host_inbox_item {
    uint8_t *buf;
    uint32_t len;
};

/* One queued outbound raft message (encoded wire bytes, heap-owned). */
struct host_outbox_item {
    uint8_t *buf;
    uint32_t len;
};

/* Per-peer send queue + its lazily-spawned sender thread. All fields are
 * guarded by the host's outbox_mu; the thread arg is &h->tx[i] itself. */
struct host_outbox {
    struct efs_raft_host *h; /* back-pointer, set at start */
    int peer;                /* raft_id this outbox serves */
    pthread_t tid;
    int started;
    int n;
    struct host_outbox_item q[HOST_OUTBOX_MAX];
    /* EFS_RAFT_OBS counters (guarded by outbox_mu; stats are diagnostic
     * only, so the sender updates rtt/getwait without the lock — worst
     * case is a torn read in the dump). */
    uint64_t st_enq;      /* queued messages */
    uint64_t st_drop;     /* dropped (queue full / shutting down) */
    uint64_t st_sent;     /* delivered + ACKed */
    uint64_t st_fail;     /* send/ACK failures (conn dropped) */
    uint32_t st_hi;       /* high-water queue depth */
    uint64_t st_rtt_us;   /* cumulative send->ACK latency */
    uint64_t st_get_us;   /* cumulative peer_conn_get wait */
    uint64_t st_get_max_us;
};

/* Apply-result ring slots per group. Power of 2. Must exceed the most
 * applies that can land inside one host_propose_wait window (the wait
 * deadline is ~400 ms and each apply is a KV WAL fsync, ~200 us, so the
 * real ceiling is ~2000; 8192 is 4x that). 96 KB per group. */
#define HOST_APPLY_RC_RING 8192
#define HOST_APPLY_RC_MASK (HOST_APPLY_RC_RING - 1)

struct host_group {
    uint8_t group;
    uint8_t hosted;
    uint32_t voters;
    uint64_t applied_saved;
    struct efs_raft *r;
    struct efs_raft_host *host; /* back-pointer, set in attach_group */
    /* Apply-result ring: (index, rc) of the last HOST_APPLY_RC_RING applied
     * entries, written by host_apply under h->mu BEFORE the raft core bumps
     * last_applied. Lets host_propose_wait return the apply layer's verdict
     * (a rejected txn PREP: intent-conflict BUSY / version STALE) instead of
     * only "the index applied". Without it a lost conflict looked like
     * success and a cross-shard txn committed with a partial intent set —
     * the peer_concurrent_hardlink nlink lost-update. */
    uint64_t arc_idx[HOST_APPLY_RC_RING];
    int32_t arc_rc[HOST_APPLY_RC_RING];
    /* EFS_RAFT_OBS: last logged term/role, for election-churn timing. */
    uint64_t obs_term;
    int obs_role;
};

/* One blocked lock waiter (10.5c-34). Stack-allocated by the waiting
 * connection thread; the queue is leader memory, never Raft state. */
struct host_lock_wait {
    struct host_lock_wait *next;
    efs_ino_t ino;
    uint64_t owner;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    uint8_t domain;
    uint8_t ltype;
    uint64_t start;
    uint64_t end;
    pthread_cond_t cv;
    int abort;
    int fenced; /* revocation barrier: never grant, reply STALE */
};

struct efs_raft_host {
    struct efsd_server *s;
    int raft_id;
    int n;
    uint64_t boot_id;
    uint64_t salt;
    struct efs_kv *kv;
    struct efs_raft_disk *disk;
    struct host_group g[HOST_NGROUPS];
    pthread_mutex_t mu;
    pthread_mutex_t read_mu; /* serializes ReadIndex; never held by the pump */
    pthread_mutex_t wait_mu; /* lock wait queue; taken after read_mu, never reverse */
    struct host_lock_wait *wait_head;
    struct host_lock_wait *wait_tail;
    pthread_t tid;
    int running;
    int started;
    pthread_t gc_tid;    /* background GC reaper (spec L7) */
    int gc_running;
    int gc_started;
    pthread_mutex_t inbox_mu;
    struct host_inbox_item inbox[HOST_INBOX_MAX];
    int inbox_n;
    /* Event-driven pump: pump_efd wakes the pump (inbox/propose/stop) between
     * its 5 ms timer ticks. It is an eventfd, NOT a condvar on h->mu, on
     * purpose: h->mu serializes ALL raft core work (tick/propose/recv, incl.
     * WAL fsync), so a network-handler wakeup that needed h->mu would queue
     * behind whatever the core is doing. eventfd write needs no lock.
     * applied_cv is broadcast by the pump after each cycle so waiters see a
     * fresh applied index without polling; used with h->mu (no I/O under
     * that wait). */
    int pump_efd;
    pthread_cond_t applied_cv;
    /* Per-peer send outboxes. host_send runs under h->mu (pump ticks and
     * proposer threads) and must NEVER do network I/O there: a dead peer's
     * synchronous send+ACK (up to HOST_SEND_IO_MS) under h->mu stalled
     * every election tick and proposal on BOTH groups — that was the
     * election churn under load (term 46 observed). host_send now only
     * encodes and queues; one lazily-spawned sender thread per peer does
     * the blocking send + empty-ACK wait, preserving per-peer FIFO order,
     * dead-conn detection and per-peer backpressure. A dead peer stalls
     * only its own sender. outbox_mu/outbox_cv guard all tx[] state;
     * tx_running=0 tells senders to drop-and-exit and host_send to drop. */
    pthread_mutex_t outbox_mu;
    pthread_cond_t outbox_cv;
    int tx_running;
    struct host_outbox tx[EFS_RAFT_MAX_PEERS];
    /* EFS_RAFT_OBS: wait_applied timeouts, pump h->mu hold high-water, and
     * the last stats-dump timestamp (ms). */
    uint64_t obs_wait_timeouts;
    uint64_t obs_pump_hold_max_us;
    uint64_t obs_last_dump_ms;
    /* EFS_RAFT_OBS: per-cycle phase maxima (us) + applies in the worst
     * cycle, so a hold spike says WHERE the time went. */
    uint64_t obs_wait_max_us;   /* max h->mu lock WAIT in the pump */
    uint64_t obs_drain_max_us;
    uint64_t obs_tick_max_us;
    uint64_t obs_persist_max_us;
    uint64_t obs_apply_max_us;
    uint64_t obs_apply_cycle_us; /* cumulative apply time, current cycle */
    uint64_t obs_apply_cnt;      /* applies in the current cycle */
    uint64_t obs_apply_max_cnt;  /* most applies in one cycle */
    /* Apply-result ring reads that found the slot overwritten/never written
     * (noop/cfg entry, or >HOST_APPLY_RC_RING applies in one wait window).
     * The waiter then returns EFS_OK — the pre-fix behavior — so a nonzero
     * sustained value means the ring is too small, not a correctness bug. */
    uint64_t obs_arc_miss;
};

static struct efs_raft_host *g_host;

static void wr64be(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t rd64be(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t now_ns(void);
static void host_stop_senders(struct efs_raft_host *h);

static int env_on(const char *name)
{
    const char *v = getenv(name);
    if (!v || !v[0] || strcmp(v, "0") == 0)
        return 0;
    return 1;
}

static uint64_t now_us_(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int raft_obs_on(void)
{
    static int v = -1;
    if (v < 0)
        v = env_on("EFS_RAFT_OBS");
    return v;
}

static uint32_t group_voters(uint8_t group, int n)
{
    if (n <= 3)
        return (1u << n) - 1u;
    if (group == EFS_RAFT_GROUP_SHARD)
        return 0x7u; /* nodes 1,2,3 = raft ids 0,1,2 */
    return 0xeu;     /* nodes 2,3,4 = raft ids 1,2,3 */
}

static int hosts_group(int raft_id, uint32_t voters)
{
    if (raft_id < 0 || raft_id >= EFS_RAFT_MAX_PEERS)
        return 0;
    return (voters & (1u << raft_id)) != 0;
}

static int peer_addr(struct efs_raft_host *h, int raft_id,
                     char *host, size_t hlen, uint16_t *port)
{
    efs_node_id_t nid;
    uint32_t i;

    if (!h || !h->s || raft_id < 0)
        return -1;
    nid = (efs_node_id_t)(raft_id + 1);
    pthread_mutex_lock(&h->s->lock);
    for (i = 0; i < h->s->node_count; i++) {
        if (h->s->nodes[i].id == nid) {
            strncpy(host, h->s->nodes[i].addr, hlen - 1);
            host[hlen - 1] = '\0';
            *port = h->s->nodes[i].port;
            pthread_mutex_unlock(&h->s->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&h->s->lock);
    return -1;
}

/* Per-peer sender: pops encoded messages FIFO and does the blocking
 * send + empty-ACK wait (dead-conn detection + backpressure) that used to
 * run under h->mu. Never touches h->mu; outbox_mu is never held across
 * I/O. On stop (tx_running=0) it drops whatever is queued and exits — the
 * server is going away and Raft retries from the next incarnation. */
static void *host_sender(void *arg)
{
    struct host_outbox *tx = arg;
    struct efs_raft_host *h = tx->h;

    for (;;) {
        uint8_t *buf;
        uint32_t len;
        char host[64];
        uint16_t port = 0;
        struct efs_conn *pc;
        uint8_t rtype = 0;
        void *reply = NULL;
        uint32_t rlen = 0;
        int i;

        pthread_mutex_lock(&h->outbox_mu);
        while (tx->n == 0 && h->tx_running)
            pthread_cond_wait(&h->outbox_cv, &h->outbox_mu);
        if (!h->tx_running) {
            for (i = 0; i < tx->n; i++)
                free(tx->q[i].buf);
            tx->n = 0;
            pthread_mutex_unlock(&h->outbox_mu);
            return NULL;
        }
        buf = tx->q[0].buf;
        len = tx->q[0].len;
        memmove(&tx->q[0], &tx->q[1], (size_t)(tx->n - 1) * sizeof(tx->q[0]));
        tx->n--;
        pthread_mutex_unlock(&h->outbox_mu);

        {
            uint64_t t0 = 0, t1 = 0;
            int obs = raft_obs_on();
            if (obs)
                t0 = now_us_();
            if (peer_addr(h, tx->peer, host, sizeof(host), &port) != 0) {
                free(buf);
                continue;
            }
            pc = server_peer_conn_get(host, port);
            if (!pc) {
                free(buf);
                continue;
            }
            if (obs) {
                uint64_t gw;
                t1 = now_us_();
                gw = t1 - t0;
                tx->st_get_us += gw;
                if (gw > tx->st_get_max_us)
                    tx->st_get_max_us = gw;
            }
            if (pc->kind == EFS_CONN_TCP && pc->fd >= 0) {
                efs_set_recv_timeout(pc->fd, HOST_SEND_IO_MS);
                efs_set_send_timeout(pc->fd, HOST_SEND_IO_MS);
            }
            if (efs_conn_send_msg(pc, EFS_MSG_RAFT, buf, len) != 0) {
                tx->st_fail++;
                server_peer_conn_drop(host, port, pc);
                free(buf);
                continue;
            }
            if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0 ||
                rtype != EFS_MSG_RAFT_REPLY) {
                tx->st_fail++;
                free(reply);
                server_peer_conn_drop(host, port, pc);
                free(buf);
                continue;
            }
            if (pc->kind == EFS_CONN_TCP && pc->fd >= 0) {
                efs_set_recv_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
                efs_set_send_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
            }
            free(reply);
            server_peer_conn_release(host, port, pc);
            free(buf);
            if (obs) {
                tx->st_sent++;
                tx->st_rtt_us += now_us_() - t1;
            }
        }
    }
}

/* Best-effort: a send failure is a dropped packet. Returning an error from
 * tick/recv aborts the remaining broadcasts and stalls elections.
 *
 * Called under h->mu from the pump (tick) and from proposer threads
 * (efs_raft_propose), so it must NEVER do network I/O: it only encodes and
 * queues to the destination peer's outbox; the per-peer sender thread does
 * the blocking send. Queue full -> drop the newest message (Raft retries
 * whatever mattered). */
static int host_send(void *net, const struct efs_raft_msg *msg)
{
    struct efs_raft_host *h = net;
    uint8_t stack[HOST_ENCODE_STACK];
    uint8_t *buf = stack;
    uint32_t cap = sizeof(stack);
    uint32_t len = 0;
    uint8_t *heap = NULL;
    struct host_outbox *tx;
    int rc;

    if (!h || !msg)
        return EFS_OK;
    if (msg->to == h->raft_id || msg->to < 0 || msg->to >= EFS_RAFT_MAX_PEERS)
        return EFS_OK;
    rc = efs_wire_raft_encode(msg, buf, cap, &len);
    if (rc == EFS_ERR_NOMEM) {
        uint32_t i;
        cap = EFS_WIRE_RAFT_HDR_LEN;
        for (i = 0; i < msg->nentries; i++)
            cap += 12u + msg->entries[i].clen;
        heap = malloc(cap);
        if (!heap)
            return EFS_OK;
        buf = heap;
        rc = efs_wire_raft_encode(msg, buf, cap, &len);
    }
    if (rc != EFS_OK) {
        free(heap);
        return EFS_OK;
    }
    if (!heap) {
        heap = malloc(len);
        if (!heap)
            return EFS_OK;
        memcpy(heap, buf, len);
    }
    tx = &h->tx[msg->to];
    pthread_mutex_lock(&h->outbox_mu);
    if (!h->tx_running || tx->n >= HOST_OUTBOX_MAX) {
        tx->st_drop++;
        pthread_mutex_unlock(&h->outbox_mu);
        free(heap);
        return EFS_OK;
    }
    tx->q[tx->n].buf = heap;
    tx->q[tx->n].len = len;
    tx->n++;
    tx->st_enq++;
    if ((uint32_t)tx->n > tx->st_hi)
        tx->st_hi = (uint32_t)tx->n;
    if (!tx->started) {
        if (efsd_pthread_create(&tx->tid, host_sender, tx) == 0)
            tx->started = 1;
        else
            tx->n--; /* no sender: drop rather than queue forever */
    }
    pthread_cond_signal(&h->outbox_cv);
    pthread_mutex_unlock(&h->outbox_mu);
    return EFS_OK;
}

static int apply_mkfs_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                          uint32_t clen, uint64_t index)
{
    uint64_t now, salt = 0;
    int rc;

    if (clen < 9)
        return EFS_OK;
    now = rd64be(cmd + 1);
    if (clen >= 17)
        salt = rd64be(cmd + 9);
    rc = efs_meta_apply_mkfs(h->kv, now, salt);
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply mkfs rc=%d index=%llu\n",
                rc, (unsigned long long)index);
        return rc;
    }
    fprintf(stderr, "raft-host: applied mkfs index=%llu salt=%llu\n",
            (unsigned long long)index, (unsigned long long)salt);
    return EFS_OK;
}

/* Same encoding as sim pack_create / apply_create_cmd. Session fencing is
 * not hosted yet; apply is create_file only. EXIST is replay (idempotent).
 * Apply always returns OK so a name clash cannot stall the log. */
static int apply_create_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                            uint32_t clen, uint64_t index)
{
    efs_ino_t parent, ino = 0;
    uint32_t mode;
    char name[EFS_MAX_NAME];
    uint8_t nl;
    struct efs_meta_attrs at;
    int rc;

    if (clen < HOST_CREATE_NAME_OFF)
        return EFS_OK;
    parent = rd64be(cmd + 2);
    mode = rd32be(cmd + 10);
    memset(&at, 0, sizeof(at));
    at.uid = rd32be(cmd + 14);
    at.gid = rd32be(cmd + 18);
    at.now = rd64be(cmd + 22);
    nl = cmd[30];
    if ((uint32_t)HOST_CREATE_NAME_OFF + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_OK;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + HOST_CREATE_NAME_OFF, nl);
    rc = efs_meta_apply_create_file(h->kv, &at, parent, mode, name, &ino);
    if (rc == EFS_ERR_EXIST) {
        struct efs_meta_dentry dent;
        if (efs_meta_apply_lookup(h->kv, parent, name, &dent) == EFS_OK)
            ino = dent.ino;
        rc = EFS_OK;
    }
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply create rc=%d index=%llu parent=%llu "
                "name=%s\n",
                rc, (unsigned long long)index, (unsigned long long)parent,
                name);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied create index=%llu parent=%llu name=%s "
            "ino=%llu\n",
            (unsigned long long)index, (unsigned long long)parent, name,
            (unsigned long long)ino);
    return EFS_OK;
}

static int apply_unlink_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                            uint32_t clen, uint64_t index)
{
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint8_t nl;
    uint64_t now;
    int rc;

    if (clen < 18)
        return EFS_OK;
    parent = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    nl = cmd[17];
    if ((uint32_t)18 + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_OK;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + 18, nl);
    rc = efs_meta_apply_unlink(h->kv, parent, name, now);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK; /* replay */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply unlink rc=%d index=%llu parent=%llu "
                "name=%s\n",
                rc, (unsigned long long)index, (unsigned long long)parent,
                name);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied unlink index=%llu parent=%llu name=%s\n",
            (unsigned long long)index, (unsigned long long)parent, name);
    return EFS_OK;
}

/* Same encoding as sim apply_setattr_cmd. Session fencing is not hosted.
 * Apply never stalls the log. */
static int apply_setattr_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    struct efs_meta_setattr sa;
    efs_ino_t ino;
    uint64_t now;
    int rc;

    if (clen < HOST_SETATTR_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    memset(&sa, 0, sizeof(sa));
    sa.expect_gen = rd64be(cmd + 17);
    sa.mask = rd32be(cmd + 25);
    sa.mode = rd32be(cmd + 29);
    sa.uid = rd32be(cmd + 33);
    sa.gid = rd32be(cmd + 37);
    rc = efs_meta_apply_setattr(h->kv, ino, now, &sa);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
        rc = EFS_OK; /* replay / stale handle after a later unlink */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply setattr rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied setattr index=%llu ino=%llu mask=%u\n",
            (unsigned long long)index, (unsigned long long)ino, sa.mask);
    return EFS_OK;
}

/* Same encoding as sim apply_utimens_cmd. Session fencing is not hosted. */
static int apply_utimens_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    struct efs_meta_utimens u;
    efs_ino_t ino;
    uint64_t now;
    int rc;

    if (clen < HOST_UTIMENS_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    memset(&u, 0, sizeof(u));
    u.expect_gen = rd64be(cmd + 17);
    u.mask = rd32be(cmd + 25);
    u.mtime = rd64be(cmd + 29);
    u.atime = rd64be(cmd + 37);
    u.mtime_gen = rd64be(cmd + 45);
    rc = efs_meta_apply_utimens(h->kv, ino, now, &u);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply utimens rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied utimens index=%llu ino=%llu mask=%u\n",
            (unsigned long long)index, (unsigned long long)ino, u.mask);
    return EFS_OK;
}

/* Same encoding as sim apply_truncate_cmd. Session fencing is not hosted. */
static int apply_truncate_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                              uint32_t clen, uint64_t index)
{
    struct efs_meta_truncate t;
    struct efs_meta_pub tail;
    efs_ino_t ino;
    uint64_t now;
    const uint8_t *q;
    int i, rc;

    if (clen < HOST_TRUNC_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    memset(&t, 0, sizeof(t));
    t.expect_gen = rd64be(cmd + 17);
    t.size = rd64be(cmd + 25);
    t.lane_mask = rd64be(cmd + 54);
    t.tail_external = (cmd[62] & HOST_TRUNC_F_TAIL_EXT) ? 1 : 0;
    if (cmd[33]) {
        if (clen < HOST_TRUNC_LEN + HOST_TRUNC_TAIL)
            return EFS_OK;
        q = cmd + HOST_TRUNC_LEN;
        memset(&tail, 0, sizeof(tail));
        tail.ino = ino;
        tail.chunk_index = rd32be(q);
        tail.new_size = t.size;
        tail.candidate_gen = rd64be(q + 4);
        tail.expected_gen = rd64be(q + 12);
        tail.coding_profile_id = rd32be(q + 20);
        q += 24;
        for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            tail.ch.nodes[i] = rd32be(q);
            q += 4;
            memcpy(tail.ch.checksums[i], q, EFS_HASH_SIZE);
            q += EFS_HASH_SIZE;
        }
        tail.now = now;
        t.tail = &tail;
    }
    rc = efs_meta_apply_truncate(h->kv, ino, now, &t);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply truncate rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied truncate index=%llu ino=%llu size=%llu\n",
            (unsigned long long)index, (unsigned long long)ino,
            (unsigned long long)t.size);
    return EFS_OK;
}

/* Host-generated lane maintenance (never client-originated, no session
 * suffix): EFS_MD_CMD_ACTIVATE_LANE on the inode group. */
static int apply_activate_lane_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                   uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    int rc;

    if (clen < HOST_ACTIVATE_LANE_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    rc = efs_meta_apply_activate_lane(h->kv, ino, cmd[9]);
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply activate-lane rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied activate-lane index=%llu ino=%llu lane=%u\n",
            (unsigned long long)index, (unsigned long long)ino,
            (unsigned)cmd[9]);
    return EFS_OK;
}

/* EFS_MD_CMD_LANE_FENCE on the lane's group: one lane's share of a
 * truncate, proposed before the inode-group truncate entry. */
static int apply_lane_fence_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    int rc;

    if (clen < HOST_LANE_FENCE_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    rc = efs_meta_apply_lane_fence(h->kv, ino, rd64be(cmd + 9), cmd[17],
                                   rd64be(cmd + 18), rd64be(cmd + 26),
                                   rd32be(cmd + 34), cmd[38]);
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply lane-fence rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied lane-fence index=%llu ino=%llu lane=%u\n",
            (unsigned long long)index, (unsigned long long)ino,
            (unsigned)cmd[17]);
    return EFS_OK;
}

/* Pump-safe: apply already holds the committed KV. Must not ReadIndex. */
static int host_apply_coord(void *user, const struct efs_txid *t,
                            uint32_t coord_shard, int *dec)
{
    struct efs_raft_host *h = user;

    if (!h || !h->kv || !t || !dec)
        return EFS_ERR_IO;
    return efs_txn_decision_get(h->kv, coord_shard, t, dec);
}

/* EFS_MD_CMD_LANE_SWEEP on the lane's group: delete the lane's chunk keys
 * (emitting one GC record per chunk on this group's anchor shard), then the
 * lane key. Proposed by the reaper for each active lane of a dead inode. */
static int apply_lane_sweep_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    int rc;

    if (clen < 18)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    rc = efs_meta_apply_lane_sweep(h->kv, ino, rd64be(cmd + 9), cmd[17]);
    if (rc != EFS_OK)
        fprintf(stderr, "raft-host: apply lane-sweep rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
    return EFS_OK;
}

/* EFS_MD_CMD_REAP_DONE on the inode's group: every active lane of the dead
 * inode was swept; clear leftover append state and the reap marker. */
static int apply_reap_done_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                               uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    int rc;

    if (clen < 17)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    rc = efs_meta_apply_reap_done(h->kv, ino, rd64be(cmd + 9));
    if (rc != EFS_OK)
        fprintf(stderr, "raft-host: apply reap-done rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
    return EFS_OK;
}

/* EFS_MD_CMD_GC_ACK on the group whose anchor shard holds the records:
 * [cnt:2][(ino:8)(gen:8)(lane:1)(ci:4)(frag:1)]*cnt — one fragment of each
 * record was deleted (or was already gone) on its target node. */
static int apply_gc_ack_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                            uint32_t clen, uint64_t index)
{
    struct efs_gc_ack_item items[16];
    uint32_t cnt, i, off;
    int rc;

    (void)index;
    if (clen < 3)
        return EFS_OK;
    cnt = (uint32_t)((cmd[1] << 8) | cmd[2]);
    if (cnt > 16 || clen < 3 + cnt * 22)
        return EFS_OK;
    off = 3;
    for (i = 0; i < cnt; i++) {
        items[i].ino = rd64be(cmd + off);
        items[i].gen = rd64be(cmd + off + 8);
        items[i].lane = cmd[off + 16];
        items[i].ci = rd32be(cmd + off + 17);
        items[i].frag = cmd[off + 21];
        off += 22;
    }
    rc = efs_meta_apply_gc_ack(h->kv, items, cnt);
    if (rc != EFS_OK)
        fprintf(stderr, "raft-host: apply gc-ack rc=%d index=%llu cnt=%u\n",
                rc, (unsigned long long)index, cnt);
    return EFS_OK;
}

/* Same layout as sim apply_append_rsv_cmd, big-endian. A zero UUID/seq
 * is the stand-in (no op-id window). A session suffix fills uuid+epoch
 * so LEASE_DROP can FENCED_HOLE that reservation (10.5c-35d). */
static int apply_append_rsv_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                uint32_t clen, uint64_t index)
{
    struct efs_opid op;
    efs_ino_t ino;
    uint64_t len, off = 0;
    int rc;

    if (clen < HOST_APPEND_RSV_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    len = rd64be(cmd + 9);
    memset(&op, 0, sizeof(op));
    memcpy(op.client_uuid, cmd + 17, EFS_OPID_UUID_LEN);
    op.session_epoch = rd32be(cmd + 33);
    op.seq = rd64be(cmd + 37);
    rc = efs_meta_apply_append_reserve(h->kv, ino, len, &op, host_apply_coord,
                                       h, &off);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_INVAL || rc == EFS_ERR_BUSY)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply append-rsv rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied append-rsv index=%llu ino=%llu\n",
            (unsigned long long)index, (unsigned long long)ino);
    return EFS_OK;
}

static int apply_append_res_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    uint64_t off;
    int outcome, rc;

    if (clen < HOST_APPEND_RES_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    off = rd64be(cmd + 9);
    outcome = (int)cmd[17];
    rc = efs_meta_apply_append_resolve(h->kv, ino, off, outcome);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply append-res rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied append-res index=%llu ino=%llu off=%llu\n",
            (unsigned long long)index, (unsigned long long)ino,
            (unsigned long long)off);
    return EFS_OK;
}

/* Same encoding as sim_dir_apply. Empty LOCAL → HASHED for the smoke.
 * Same-group leftover migrate stays this apply. A leftover whose HASHED
 * shard is on the other group is a txn from host_dir_migrate (hashed PUT
 * in the dest log). I8: hashed live/tombstone skips the PUT. */
static int apply_dir_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                         uint32_t clen, uint64_t index)
{
    efs_ino_t dir;
    int rc = EFS_ERR_PROTO;

    if (clen < HOST_DIR_LEN)
        return EFS_OK;
    dir = rd64be(cmd + 2);
    switch (cmd[1]) {
    case EFS_MD_DIR_BEGIN:
        rc = efs_meta_dir_begin_split(h->kv, dir);
        break;
    case EFS_MD_DIR_MIGRATE:
        rc = efs_meta_dir_migrate_one(h->kv, dir);
        break;
    case EFS_MD_DIR_FINISH:
        rc = efs_meta_dir_finish_hashed(h->kv, dir);
        break;
    default:
        return EFS_OK;
    }
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_INVAL || rc == EFS_ERR_BUSY)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply dir rc=%d index=%llu ino=%llu kind=%u\n",
                rc, (unsigned long long)index, (unsigned long long)dir,
                (unsigned)cmd[1]);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied dir index=%llu ino=%llu kind=%u\n",
            (unsigned long long)index, (unsigned long long)dir,
            (unsigned)cmd[1]);
    return EFS_OK;
}

static void host_lock_wait_drop_session(struct efs_raft_host *h,
                                        const uint8_t uuid[EFS_OPID_UUID_LEN],
                                        uint32_t epoch);
static void lock_wait_signal_locked(struct efs_raft_host *h, efs_ino_t ino);

/* Same encoding as sim_sess_apply. 10.5c-35a hosts CREATE / REGISTER /
 * ESTABLISH; 10.5c-35c hosts the revocation barrier (BEGIN / FENCE_LOC /
 * ACK / FINISH / LEASE_DROP). Last close reclaims a nlink=0 inode (I19).
 * Apply never stalls the log. */
static int apply_session_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch = 0;
    uint32_t shard = 0;
    efs_ino_t ino = 0;
    uint64_t gen = 0;
    int rc = EFS_ERR_PROTO;

    if (clen < HOST_SESS_HDR_LEN)
        return EFS_OK;
    memcpy(uuid, cmd + 2, EFS_OPID_UUID_LEN);
    switch (cmd[1]) {
    case EFS_MD_SESS_CREATE:
        if (clen < HOST_SESS_CREATE_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        rc = efs_session_create(h->kv, uuid, epoch);
        break;
    case EFS_MD_SESS_REGISTER:
        if (clen < HOST_SESS_REG_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        shard = rd32be(cmd + 22);
        rc = efs_session_register(h->kv, uuid, epoch, shard);
        break;
    case EFS_MD_SESS_ESTABLISH:
        if (clen < HOST_SESS_REG_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        shard = rd32be(cmd + 22);
        rc = efs_session_establish(h->kv, shard, uuid, epoch);
        break;
    case EFS_MD_SESS_BEGIN:
        rc = efs_session_begin_fence(h->kv, uuid);
        break;
    case EFS_MD_SESS_FENCE_LOC:
        if (clen < HOST_SESS_REG_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        shard = rd32be(cmd + 22);
        rc = efs_session_fence_local(h->kv, shard, uuid, epoch);
        /* Abort in-memory waiters of the fenced epoch now (not only at
         * LEASE_DROP): a queued waiter must not GRANT after FENCE_LOC. */
        if (rc == EFS_OK && epoch > 0)
            host_lock_wait_drop_session(h, uuid, epoch - 1);
        break;
    case EFS_MD_SESS_ACK:
        if (clen < HOST_SESS_CREATE_LEN)
            return EFS_OK;
        shard = rd32be(cmd + 18);
        rc = efs_session_ack_fence(h->kv, uuid, shard);
        break;
    case EFS_MD_SESS_FINISH:
        rc = efs_session_finish_fence(h->kv, uuid);
        break;
    case EFS_MD_SESS_LEASE_DROP:
        if (clen < HOST_SESS_REG_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        shard = rd32be(cmd + 22);
        rc = efs_lease_drop_session(h->kv, shard, uuid, epoch);
        if (rc == EFS_OK || rc == EFS_ERR_NOT_FOUND) {
            int r2 = efs_lock_drop_session(h->kv, shard, uuid, epoch);
            int r3 = efs_meta_apply_append_drop_session(h->kv, shard, uuid,
                                                        epoch);

            if (r2 != EFS_OK && r2 != EFS_ERR_NOT_FOUND)
                rc = r2;
            else if (r3 != EFS_OK && r3 != EFS_ERR_NOT_FOUND)
                rc = r3;
            else if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
        }
        host_lock_wait_drop_session(h, uuid, epoch);
        break;
    case EFS_MD_SESS_LEASE_OPEN:
        if (clen < HOST_SESS_LEASE_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        ino = rd64be(cmd + 22);
        gen = rd64be(cmd + 30);
        rc = efs_lease_open(h->kv, ino, gen, uuid, epoch);
        break;
    case EFS_MD_SESS_LEASE_CLOSE:
        if (clen < HOST_SESS_LEASE_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        ino = rd64be(cmd + 22);
        gen = rd64be(cmd + 30);
        rc = efs_lease_close(h->kv, ino, gen, uuid, epoch);
        if (rc == EFS_OK) {
            int r2;
            /* Last close of the inode drops its locks: the kernel never
             * relays a flock UNLOCK on close, so the last-lease edge is the
             * only close-to-release signal (§7.6). */
            if (efs_lease_any(h->kv, ino, gen) == 0) {
                int lr = efs_lock_drop_file(h->kv, ino, gen);
                int dr;

                if (lr != EFS_OK && lr != EFS_ERR_NOT_FOUND)
                    rc = lr;
                /* Same edge for orphaned append reservations: the writer
                 * that reserved them is gone, so they can never complete —
                 * resolve as holes or the frontier wedges and the records
                 * accumulate without bound (§7.3 ABORTED_HOLE). */
                dr = efs_meta_apply_append_drain_file(h->kv, ino);
                if (dr != EFS_OK && dr != EFS_ERR_NOT_FOUND)
                    rc = dr;
                pthread_mutex_lock(&h->wait_mu);
                lock_wait_signal_locked(h, ino);
                pthread_mutex_unlock(&h->wait_mu);
            }
            r2 = efs_meta_apply_reclaim(h->kv, ino);
            if (r2 != EFS_OK && r2 != EFS_ERR_BUSY && r2 != EFS_ERR_NOT_FOUND)
                rc = r2;
        }
        break;
    case EFS_MD_SESS_RECLAIM:
        if (clen < 26)
            return EFS_OK;
        ino = rd64be(cmd + 18);
        rc = efs_meta_apply_reclaim(h->kv, ino);
        break;
    default:
        return EFS_OK;
    }
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_INVAL || rc == EFS_ERR_BUSY ||
        rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr,
                "raft-host: apply session rc=%d index=%llu ino=%llu kind=%u\n",
                rc, (unsigned long long)index, (unsigned long long)ino,
                (unsigned)cmd[1]);
        return EFS_OK;
    }
    fprintf(stderr,
            "raft-host: applied session index=%llu kind=%u shard=%u ino=%llu\n",
            (unsigned long long)index, (unsigned)cmd[1], shard,
            (unsigned long long)ino);
    return EFS_OK;
}

/* Same encoding as sim_lock_apply. Conflict/cap/stale never stall the
 * log; the propose path returns BUSY to the client. Blocking wait
 * queues stay leader memory (not hosted). */
static int apply_lock_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                          uint32_t clen, uint64_t index)
{
    struct efs_lock_req r;
    const uint8_t *p;
    int rc = EFS_ERR_PROTO;

    if (clen < HOST_LOCK_LEN)
        return EFS_OK;
    memset(&r, 0, sizeof(r));
    r.ino = rd64be(cmd + 2);
    r.generation = rd64be(cmd + 10);
    r.domain = cmd[18];
    r.type = cmd[19];
    r.start = rd64be(cmd + 20);
    r.end = rd64be(cmd + 28);
    r.owner.kind = cmd[36];
    r.owner.id = rd64be(cmd + 37);
    p = cmd + 45;
    memcpy(r.owner.uuid, p, EFS_OPID_UUID_LEN);
    r.owner.epoch = rd32be(p + EFS_OPID_UUID_LEN);
    if (cmd[1] == EFS_MD_LOCK_GRANT)
        rc = efs_lock_grant(h->kv, &r);
    else if (cmd[1] == EFS_MD_LOCK_RELEASE)
        rc = efs_lock_release(h->kv, &r);
    else
        return EFS_OK;
    if (rc == EFS_ERR_AGAIN || rc == EFS_ERR_NOLCK || rc == EFS_ERR_NOT_FOUND ||
        rc == EFS_ERR_INVAL || rc == EFS_ERR_BUSY || rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr,
                "raft-host: apply lock rc=%d index=%llu ino=%llu kind=%u\n",
                rc, (unsigned long long)index, (unsigned long long)r.ino,
                (unsigned)cmd[1]);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied lock index=%llu ino=%llu kind=%u\n",
            (unsigned long long)index, (unsigned long long)r.ino,
            (unsigned)cmd[1]);
    return EFS_OK;
}

/* Same encoding as sim apply_publish_cmd. Session fencing is not hosted. */
static int apply_publish_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    struct efs_meta_pub p;
    const uint8_t *q;
    int i, rc;

    if (clen < HOST_PUBLISH_LEN)
        return EFS_OK;
    memset(&p, 0, sizeof(p));
    p.ino = rd64be(cmd + 1);
    p.chunk_index = rd32be(cmd + 9);
    p.new_size = rd64be(cmd + 13);
    p.now = rd64be(cmd + 21);
    q = cmd + 29;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = rd32be(q);
        q += 4;
        memcpy(p.ch.checksums[i], q, EFS_HASH_SIZE);
        q += EFS_HASH_SIZE;
    }
    q += EFS_OPID_UUID_LEN + 4;
    p.candidate_gen = rd64be(q);
    p.expected_gen = rd64be(q + 8);
    p.content_epoch = rd64be(q + 16);
    p.coding_profile_id = rd32be(q + 24);
    q += 28;
    p.inode_gen = rd64be(q);
    p.mtime_gen = rd64be(q + 8);
    p.lane_local = (q[16] & HOST_PUB_F_LANE_LOCAL) ? 1 : 0;
    rc = efs_meta_apply_publish(h->kv, &p);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply publish rc=%d index=%llu ino=%llu ci=%u\n",
                rc, (unsigned long long)index, (unsigned long long)p.ino,
                p.chunk_index);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied publish index=%llu ino=%llu ci=%u\n",
            (unsigned long long)index, (unsigned long long)p.ino, p.chunk_index);
    return EFS_OK;
}

/* Same encoding as sim_txn_apply. Returns the apply layer's real verdict
 * (a rejected PREP is intent-conflict BUSY / version STALE); host_apply
 * records it in the group ring and is what keeps txn apply from stalling
 * the log. */
static int apply_txn_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                         uint32_t clen)
{
    struct efs_txid t;
    uint32_t shard, off;
    int rc = EFS_ERR_PROTO, dec, kind, op;
    uint64_t expected;
    uint32_t klen;
    const uint8_t *key, *val;
    uint32_t vlen;
    struct efs_txn_parts p;
    struct efs_txn_reduce red;

    memset(&t, 0, sizeof(t));
    memset(&p, 0, sizeof(p));
    switch (cmd[0]) {
    case EFS_MD_CMD_PREPARE:
        if (clen < 1 + 1 + 16 + 1)
            break;
        kind = cmd[1];
        memcpy(t.bytes, cmd + 2, 16);
        p.n = cmd[18];
        if (p.n == 0 || p.n > EFS_TXN_MAX_PART)
            break;
        off = 19;
        if (clen < off + (uint32_t)p.n * 4u + 2u)
            break;
        {
            uint8_t i;
            for (i = 0; i < p.n; i++)
                p.shard[i] = rd32be(cmd + off + (uint32_t)i * 4u);
        }
        off += (uint32_t)p.n * 4u;
        klen = ((uint32_t)cmd[off] << 8) | cmd[off + 1];
        off += 2;
        if (klen > EFS_KV_KEY_MAX || clen < off + klen)
            break;
        key = cmd + off;
        off += klen;
        if (kind == EFS_TXN_EXCL) {
            if (clen < off + 8 + 1 + 4)
                break;
            expected = rd64be(cmd + off);
            op = cmd[off + 8];
            vlen = rd32be(cmd + off + 9);
            off += 13;
            if (clen < off + vlen)
                break;
            val = cmd + off;
            rc = efs_txn_prepare_excl(h->kv, &t, &p, key, klen, expected, op,
                                      val, vlen);
        } else if (kind == EFS_TXN_GUARD) {
            if (clen < off + 8)
                break;
            expected = rd64be(cmd + off);
            rc = efs_txn_prepare_guard(h->kv, &t, &p, key, klen, expected);
        } else if (kind == EFS_TXN_REDUCE) {
            if (clen < off + 24)
                break;
            red.max_end = rd64be(cmd + off);
            red.max_mtime = rd64be(cmd + off + 8);
            red.max_ctime = rd64be(cmd + off + 16);
            rc = efs_txn_prepare_reduce(h->kv, &t, &p, key, klen, &red);
        }
        break;
    case EFS_MD_CMD_DECIDE:
        if (clen < 1 + 16 + 4 + 1)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32be(cmd + 17);
        dec = cmd[21];
        rc = efs_txn_decide(h->kv, shard, &t, dec);
        break;
    case EFS_MD_CMD_RESOLVE:
        if (clen < 1 + 16 + 4 + 1)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32be(cmd + 17);
        dec = cmd[21];
        rc = efs_txn_resolve(h->kv, &t, shard, dec);
        break;
    case EFS_MD_CMD_DROP:
        if (clen < 1 + 16 + 4)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32be(cmd + 17);
        rc = efs_txn_drop(h->kv, &t, shard);
        break;
    default:
        return EFS_OK;
    }
    return rc;
}

static int host_apply_dispatch(struct efs_raft_host *h, uint64_t index,
                               const uint8_t *cmd, uint32_t clen);

static int host_apply(void *app, uint64_t index, uint64_t term,
                      const uint8_t *cmd, uint32_t clen)
{
    struct host_group *g = app;
    struct efs_raft_host *h = g->host;
    uint64_t a0 = 0;
    int rc, ret;

    (void)term;
    if (!h || !h->kv || !cmd || clen == 0)
        return EFS_OK;
    if (raft_obs_on())
        a0 = now_us_();
    rc = host_apply_dispatch(h, index, cmd, clen);
    /* Record the apply verdict BEFORE the raft core bumps last_applied, so a
     * waiter that observes applied >= index always finds the slot. The pump
     * holds h->mu across apply_committed, and host_propose_wait reads the
     * ring under h->mu, so this is race-free. */
    g->arc_idx[index & HOST_APPLY_RC_MASK] = index;
    g->arc_rc[index & HOST_APPLY_RC_MASK] = rc;
    /* Txn apply never stalls the log: a conflict verdict rides the ring to
     * the proposer, it is not a raft-core error. Other cmds keep their
     * historic return (halt-on-error for the few that can fail). */
    ret = (cmd[0] == EFS_MD_CMD_PREPARE || cmd[0] == EFS_MD_CMD_DECIDE ||
           cmd[0] == EFS_MD_CMD_RESOLVE || cmd[0] == EFS_MD_CMD_DROP)
              ? EFS_OK
              : rc;
    if (a0) {
        h->obs_apply_cnt++;
        h->obs_apply_cycle_us += now_us_() - a0;
    }
    return ret;
}

static int host_apply_dispatch(struct efs_raft_host *h, uint64_t index,
                               const uint8_t *cmd, uint32_t clen)
{
    if (cmd[0] == EFS_MD_CMD_MKFS)
        return apply_mkfs_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_CREATE)
        return apply_create_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_UNLINK)
        return apply_unlink_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_PUBLISH)
        return apply_publish_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_SETATTR)
        return apply_setattr_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_UTIMENS)
        return apply_utimens_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_TRUNCATE)
        return apply_truncate_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_ACTIVATE_LANE)
        return apply_activate_lane_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_LANE_FENCE)
        return apply_lane_fence_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_APPEND_RSV)
        return apply_append_rsv_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_APPEND_RES)
        return apply_append_res_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_DIR)
        return apply_dir_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_SESSION)
        return apply_session_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_LOCK)
        return apply_lock_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_LANE_SWEEP)
        return apply_lane_sweep_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_REAP_DONE)
        return apply_reap_done_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_GC_ACK)
        return apply_gc_ack_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_PREPARE || cmd[0] == EFS_MD_CMD_DECIDE ||
        cmd[0] == EFS_MD_CMD_RESOLVE || cmd[0] == EFS_MD_CMD_DROP)
        return apply_txn_cmd(h, cmd, clen);
    return EFS_OK;
}

static struct efs_raft *group_raft(struct efs_raft_host *h, uint8_t group)
{
    int i;
    for (i = 0; i < HOST_NGROUPS; i++) {
        if (h->g[i].hosted && h->g[i].group == group)
            return h->g[i].r;
    }
    return NULL;
}

static struct host_group *group_slot(struct efs_raft_host *h, uint8_t group)
{
    int i;
    for (i = 0; i < HOST_NGROUPS; i++) {
        if (h->g[i].group == group)
            return &h->g[i];
    }
    return NULL;
}

static int host_hosts(struct efs_raft_host *h, uint8_t group)
{
    return group_raft(h, group) != NULL;
}

static int host_hosts_all(struct efs_raft_host *h, const uint8_t *g, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (!host_hosts(h, g[i]))
            return 0;
    return 1;
}

static void bounce_add(uint8_t *gs, int *ngs, int cap, uint8_t g)
{
    int j;

    for (j = 0; j < *ngs; j++) {
        if (gs[j] == g)
            return;
    }
    if (*ngs < cap)
        gs[(*ngs)++] = g;
}

/* First peer other than self (and skip) that votes in every listed group.
 * Dual-hosts of {0,2} are raft ids 1 and 2 at n=4. Never returns self. */
static int host_pick_peer(struct efs_raft_host *h, const uint8_t *groups, int ng,
                          int skip)
{
    int rid, i;

    for (rid = 0; rid < h->n; rid++) {
        int ok = 1;
        if (rid == h->raft_id || rid == skip)
            continue;
        for (i = 0; i < ng; i++) {
            struct host_group *s = group_slot(h, groups[i]);
            uint32_t voters = s ? s->voters : group_voters(groups[i], h->n);
            if (!hosts_group(rid, voters)) {
                ok = 0;
                break;
            }
        }
        if (ok)
            return rid;
    }
    return -1;
}

/* Both scratch groups. HASHED used_shards / file lanes can sit on the
 * group this replica does not host; bounce to a dual-host instead of
 * returning NOT_PRIMARY to the client. */
static void host_need_both(uint8_t *need)
{
    need[0] = EFS_RAFT_GROUP_SHARD;
    need[1] = EFS_RAFT_GROUP_SHARD2;
}

static int host_rpc_submit(struct efs_raft_host *h, int rid, uint8_t group,
                           const uint8_t *cmd, uint32_t clen,
                           struct efs_msg_raft_mkfs_reply *rep)
{
    uint8_t payload[1 + HOST_CMD_MAX];
    uint32_t plen;
    char host[64];
    uint16_t port = 0;
    struct efs_conn *pc;
    uint8_t rtype = 0;
    void *reply = NULL;
    uint32_t rlen = 0;

    memset(rep, 0, sizeof(*rep));
    if (rid < 0 || rid == h->raft_id || clen > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    payload[0] = group;
    if (clen)
        memcpy(payload + 1, cmd, clen);
    plen = 1u + clen;
    if (peer_addr(h, rid, host, sizeof(host), &port) != 0)
        return EFS_ERR_NOT_PRIMARY;
    pc = server_peer_conn_get(host, port);
    if (!pc)
        return EFS_ERR_BUSY;
    if (efs_conn_send_msg(pc, EFS_MSG_RAFT_MKFS, payload, plen) != 0) {
        server_peer_conn_drop(host, port, pc);
        return EFS_ERR_IO;
    }
    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0 ||
        rtype != EFS_MSG_RAFT_MKFS_REPLY || rlen < sizeof(*rep)) {
        free(reply);
        server_peer_conn_drop(host, port, pc);
        return EFS_ERR_IO;
    }
    memcpy(rep, reply, sizeof(*rep));
    free(reply);
    server_peer_conn_release(host, port, pc);
    return EFS_OK;
}

/* Submit cmd (or ReadIndex if clen==0) to the group's leader. Never targets
 * self. prefer_rid is a hint; NOT_PRIMARY replies retry leader_hint. */
static int host_remote_cmd(struct efs_raft_host *h, uint8_t group,
                           const uint8_t *cmd, uint32_t clen,
                           struct efs_msg_raft_mkfs_reply *rep, int prefer_rid)
{
    int attempt, rid, skip = -1, rc;

    for (attempt = 0; attempt < h->n + 2; attempt++) {
        rid = prefer_rid;
        if (rid < 0 || rid == h->raft_id || rid == skip)
            rid = host_pick_peer(h, &group, 1, skip);
        if (rid < 0 || rid == h->raft_id)
            return EFS_ERR_NOT_PRIMARY;
        rc = host_rpc_submit(h, rid, group, cmd, clen, rep);
        if (rc != EFS_OK) {
            skip = rid;
            prefer_rid = -1;
            continue;
        }
        if (rep->rc == EFS_OK)
            return EFS_OK;
        if (rep->rc == EFS_ERR_NOT_PRIMARY && rep->leader_hint >= 0 &&
            rep->leader_hint != h->raft_id && rep->leader_hint != rid) {
            prefer_rid = rep->leader_hint;
            skip = rid;
            continue;
        }
        return rep->rc != 0 ? rep->rc : EFS_ERR_NOT_PRIMARY;
    }
    return EFS_ERR_NOT_PRIMARY;
}

static int host_wait_applied(struct efs_raft_host *h, uint8_t group,
                             uint64_t idx, int *leader_hint);
static void host_pump_kick(struct efs_raft_host *h);

static void host_deadline_us(struct timespec *ts, long us)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += us / 1000000;
    ts->tv_nsec += (us % 1000000) * 1000;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int host_past_deadline(const struct timespec *end)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec != end->tv_sec)
        return now.tv_sec > end->tv_sec;
    return now.tv_nsec >= end->tv_nsec;
}

/* Drop h->mu while waiting: the pump must tick for heartbeats to land.
 * Caller serializes with read_mu — but read_mu is NEVER held across the
 * synchronous follower forward (host_remote_cmd is a blocking peer RPC):
 * the peer's submit handler needs its own read_mu, so holding ours while
 * waiting on a peer that waits on a third node's read_mu closes a
 * cross-node deadlock cycle (observed: 003->004->005->003, all lookups
 * wedged for minutes). The forward touches no local read-round state, so
 * dropping read_mu for it is safe; the leader-side read_begin/read_ready
 * round below stays serialized by the caller's read_mu. */
static int host_read_index(struct efs_raft_host *h, uint8_t group,
                           int *leader_hint)
{
    int begun = 0;
    int lid = -1;

    if (leader_hint)
        *leader_hint = -1;
    pthread_mutex_lock(&h->mu);
    {
        struct efs_raft *r = group_raft(h, group);
        if (!r) {
            pthread_mutex_unlock(&h->mu);
            return EFS_ERR_NOT_PRIMARY;
        }
        lid = efs_raft_leader(r);
        if (leader_hint)
            *leader_hint = lid;
        if (efs_raft_role(r) != EFS_RAFT_LEADER) {
            pthread_mutex_unlock(&h->mu);
            {
                struct efs_msg_raft_mkfs_reply rep;
                int rc;
                pthread_mutex_unlock(&h->read_mu);
                rc = host_remote_cmd(h, group, NULL, 0, &rep, lid);
                if (rc == EFS_OK)
                    rc = host_wait_applied(h, group, rep.index, leader_hint);
                pthread_mutex_lock(&h->read_mu);
                if (rc != EFS_OK)
                    return rc;
                if (leader_hint && rep.leader_hint >= 0)
                    *leader_hint = rep.leader_hint;
                return EFS_OK;
            }
        }
    }
    pthread_mutex_unlock(&h->mu);

    {
        struct timespec end;
        host_deadline_us(&end, (long)HOST_READ_TRIES * HOST_TICK_US);
        pthread_mutex_lock(&h->mu);
        for (;;) {
            struct efs_raft *r = group_raft(h, group);
            int rc;

            if (!r) {
                pthread_mutex_unlock(&h->mu);
                return EFS_ERR_NOT_PRIMARY;
            }
            if (leader_hint)
                *leader_hint = efs_raft_leader(r);
            if (efs_raft_role(r) != EFS_RAFT_LEADER) {
                lid = efs_raft_leader(r);
                pthread_mutex_unlock(&h->mu);
                if (lid == h->raft_id)
                    return EFS_ERR_NOT_PRIMARY;
                {
                    struct efs_msg_raft_mkfs_reply rep;
                    int rc2;
                    /* Demoted mid-wait: forward without read_mu (see the
                     * function-header comment). */
                    pthread_mutex_unlock(&h->read_mu);
                    rc2 = host_remote_cmd(h, group, NULL, 0, &rep, lid);
                    if (rc2 == EFS_OK)
                        rc2 = host_wait_applied(h, group, rep.index,
                                                leader_hint);
                    pthread_mutex_lock(&h->read_mu);
                    if (rc2 != EFS_OK)
                        return rc2;
                    if (leader_hint && rep.leader_hint >= 0)
                        *leader_hint = rep.leader_hint;
                    return EFS_OK;
                }
            }
            if (!begun) {
                rc = efs_raft_read_begin(r);
                begun = 1;
                if (rc != EFS_OK) {
                    pthread_mutex_unlock(&h->mu);
                    return rc;
                }
            }
            if (efs_raft_read_ready(r)) {
                pthread_mutex_unlock(&h->mu);
                return EFS_OK;
            }
            if (host_past_deadline(&end)) {
                pthread_mutex_unlock(&h->mu);
                return EFS_ERR_BUSY;
            }
            /* read_ready lands when heartbeat replies arrive — the pump
             * broadcasts applied_cv at the end of that cycle. */
            pthread_cond_timedwait(&h->applied_cv, &h->mu, &end);
        }
    }
}

static int host_wait_applied(struct efs_raft_host *h, uint8_t group,
                             uint64_t idx, int *leader_hint)
{
    struct timespec end;

    /* Same overall budget as the old poll loop (HOST_READ_TRIES x
     * HOST_TICK_US), but the pump's applied_cv broadcast wakes us the
     * moment the index applies instead of up to 5 ms later. */
    host_deadline_us(&end, (long)HOST_READ_TRIES * HOST_TICK_US);
    pthread_mutex_lock(&h->mu);
    for (;;) {
        struct efs_raft *r = group_raft(h, group);
        if (!r) {
            /* No local replica: the leader already waited in submit. */
            pthread_mutex_unlock(&h->mu);
            return EFS_OK;
        }
        if (leader_hint)
            *leader_hint = efs_raft_leader(r);
        if (efs_raft_applied(r) >= idx) {
            pthread_mutex_unlock(&h->mu);
            return EFS_OK;
        }
        if (host_past_deadline(&end)) {
            h->obs_wait_timeouts++;
            pthread_mutex_unlock(&h->mu);
            return EFS_ERR_BUSY;
        }
        pthread_cond_timedwait(&h->applied_cv, &h->mu, &end);
    }
}

static int host_propose(struct efs_raft_host *h, uint8_t group,
                        const uint8_t *cmd, uint32_t clen, uint64_t *idx,
                        int *leader_hint)
{
    struct efs_raft *r;
    int rc;
    int lid = -1;
    struct efs_msg_raft_mkfs_reply rep;

    pthread_mutex_lock(&h->mu);
    r = group_raft(h, group);
    if (r) {
        lid = efs_raft_leader(r);
        if (leader_hint)
            *leader_hint = lid;
        if (efs_raft_role(r) == EFS_RAFT_LEADER) {
            rc = efs_raft_propose(r, cmd, clen, idx);
            pthread_mutex_unlock(&h->mu);
            /* New entry pending: pump ships AppendEntries immediately.
             * (After the unlock: propose already broadcast under h->mu;
             * the kick just makes the next cycle prompt.) */
            host_pump_kick(h);
            return rc;
        }
    }
    pthread_mutex_unlock(&h->mu);
    /* Not the leader: forward to it. host_remote_cmd is a blocking peer
     * RPC, and the peer's submit handler needs its own read_mu — holding
     * our read_mu across this wait closes a cross-node deadlock cycle
     * (same rule as host_read_index). The command is already fully formed
     * and the Raft log + apply-side validation order it against any op
     * that slips in here, so dropping read_mu is safe. */
    pthread_mutex_unlock(&h->read_mu);
    rc = host_remote_cmd(h, group, cmd, clen, &rep, lid);
    pthread_mutex_lock(&h->read_mu);
    if (rc != EFS_OK)
        return rc;
    if (idx)
        *idx = rep.index;
    if (leader_hint && rep.leader_hint >= 0)
        *leader_hint = rep.leader_hint;
    return EFS_OK;
}

/* h->mu held. The apply layer's verdict for log index idx, read from the
 * group's apply-result ring. EFS_OK when the slot was never written or was
 * already overwritten (a noop/cfg entry, or more than HOST_APPLY_RC_RING
 * applies inside one wait window — counted in obs_arc_miss). */
static int host_apply_rc_locked(struct efs_raft_host *h, uint8_t group,
                                uint64_t idx)
{
    struct host_group *g = group_slot(h, group);
    uint64_t s;

    if (!g)
        return EFS_OK; /* not hosted: the leader's submit reply carried it */
    s = idx & HOST_APPLY_RC_MASK;
    if (g->arc_idx[s] != idx) {
        h->obs_arc_miss++;
        return EFS_OK;
    }
    return g->arc_rc[s];
}

static int host_propose_wait(struct efs_raft_host *h, uint8_t group,
                             const uint8_t *cmd, uint32_t clen, int *hint)
{
    uint64_t idx = 0;
    int rc;

    rc = host_propose(h, group, cmd, clen, &idx, hint);
    if (rc != EFS_OK)
        return rc;
    rc = host_wait_applied(h, group, idx, hint);
    if (rc != EFS_OK)
        return rc;
    /* The index applied; the apply layer's verdict rides the per-group ring.
     * A rejected txn PREP (intent-conflict BUSY / version STALE) must reach
     * the proposer — before this, host_wait_applied only proved the index
     * applied, so a lost conflict looked like success and a cross-shard txn
     * committed with a partial intent set (the nlink lost-update). A
     * forwarded command never reaches here with a conflict: host_propose's
     * forward branch returns the leader's submit reply rc directly. */
    pthread_mutex_lock(&h->mu);
    rc = host_apply_rc_locked(h, group, idx);
    pthread_mutex_unlock(&h->mu);
    return rc;
}

static int host_inode_rpc_peer(struct efs_raft_host *h, int rid, uint8_t req_type,
                               const void *req, uint32_t reqlen,
                               uint8_t reply_type, void *out, uint32_t outlen)
{
    char host[64];
    uint16_t port = 0;
    struct efs_conn *pc;
    uint8_t rtype = 0;
    void *reply = NULL;
    uint32_t rlen = 0;

    if (rid < 0 || rid == h->raft_id || !req || !out || outlen == 0)
        return -1;
    if (peer_addr(h, rid, host, sizeof(host), &port) != 0)
        return -1;
    pc = server_peer_conn_get(host, port);
    if (!pc)
        return -1;
    if (efs_conn_send_msg(pc, req_type, req, reqlen) != 0) {
        server_peer_conn_drop(host, port, pc);
        return -1;
    }
    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0) {
        server_peer_conn_drop(host, port, pc);
        return -1;
    }
    if (rtype != reply_type || rlen < outlen) {
        free(reply);
        server_peer_conn_drop(host, port, pc);
        return -1;
    }
    memcpy(out, reply, outlen);
    free(reply);
    server_peer_conn_release(host, port, pc);
    return 0;
}

/* Bounce an inode RPC to a peer that hosts every listed group. Never
 * targets self, so a dual-host that hosts all groups never forwards
 * (no 1↔2 loop). Caller must not hold read_mu. */
static void host_inode_forward(struct efs_raft_host *h, uint8_t req_type,
                               const void *req, uint32_t reqlen,
                               uint8_t reply_type,
                               struct efs_msg_inode_reply *out,
                               const uint8_t *groups, int ng)
{
    int tries, rid, skip = -1;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_NOT_PRIMARY;
    for (tries = 0; tries < h->n; tries++) {
        rid = host_pick_peer(h, groups, ng, skip);
        if (rid < 0)
            return;
        if (host_inode_rpc_peer(h, rid, req_type, req, reqlen, reply_type,
                                out, sizeof(*out)) != 0) {
            skip = rid;
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            continue;
        }
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            return;
        skip = rid;
    }
}

static void host_fwd_create(struct efs_raft_host *h, efs_ino_t parent,
                            const char *name, uint32_t mode, uint32_t uid,
                            uint32_t gid, struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_create req;

    memset(&req, 0, sizeof(req));
    req.parent = parent;
    strncpy(req.name, name, EFS_MAX_NAME - 1);
    req.mode = mode;
    req.uid = uid;
    req.gid = gid;
    host_inode_forward(h, EFS_MSG_INODE_CREATE, &req, sizeof(req),
                       EFS_MSG_INODE_CREATE_REPLY, out, groups, ng);
}

static void host_fwd_lookup(struct efs_raft_host *h, efs_ino_t parent,
                            const char *name, struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_lookup req;

    memset(&req, 0, sizeof(req));
    req.parent = parent;
    strncpy(req.name, name, EFS_MAX_NAME - 1);
    host_inode_forward(h, EFS_MSG_INODE_LOOKUP, &req, sizeof(req),
                       EFS_MSG_INODE_LOOKUP_REPLY, out, groups, ng);
}

static void host_fwd_getattr(struct efs_raft_host *h, efs_ino_t ino,
                             struct efs_msg_inode_reply *out,
                             const uint8_t *groups, int ng)
{
    struct efs_msg_inode_getattr req;

    memset(&req, 0, sizeof(req));
    req.ino = ino;
    host_inode_forward(h, EFS_MSG_INODE_GETATTR, &req, sizeof(req),
                       EFS_MSG_INODE_GETATTR_REPLY, out, groups, ng);
}

static void host_fwd_hold(struct efs_raft_host *h, efs_ino_t ino, uint32_t flags,
                          uint64_t owner, const uint8_t *sess_uuid,
                          uint32_t sess_epoch, struct efs_msg_inode_reply *out,
                          const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_hold) + EFS_SESS_WIRE_LEN];
    struct efs_msg_inode_hold *req = (struct efs_msg_inode_hold *)buf;
    uint32_t slen = sizeof(*req);

    memset(buf, 0, sizeof(buf));
    req->ino = ino;
    req->flags = flags;
    req->owner = owner;
    if (sess_uuid) {
        memcpy(buf + slen, sess_uuid, EFS_OPID_UUID_LEN);
        memcpy(buf + slen + EFS_OPID_UUID_LEN, &sess_epoch, 4);
        slen += EFS_SESS_WIRE_LEN;
    }
    host_inode_forward(h, EFS_MSG_INODE_HOLD, buf, slen,
                       EFS_MSG_INODE_HOLD_REPLY, out, groups, ng);
}

static void host_fwd_flock(struct efs_raft_host *h, efs_ino_t ino, uint32_t op,
                           uint64_t owner, uint64_t start, uint64_t end,
                           const uint8_t *sess_uuid, uint32_t sess_epoch,
                           struct efs_msg_inode_reply *out,
                           const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_flock) + EFS_FLOCK_RANGE_LEN +
                EFS_SESS_WIRE_LEN];
    struct efs_msg_inode_flock *req = (struct efs_msg_inode_flock *)buf;
    uint32_t slen;

    memset(buf, 0, sizeof(buf));
    req->ino = ino;
    req->op = op;
    req->owner = owner;
    memcpy(buf + sizeof(*req), &start, 8);
    memcpy(buf + sizeof(*req) + 8, &end, 8);
    slen = sizeof(*req) + EFS_FLOCK_RANGE_LEN;
    if (sess_uuid) {
        memcpy(buf + slen, sess_uuid, EFS_OPID_UUID_LEN);
        memcpy(buf + slen + EFS_OPID_UUID_LEN, &sess_epoch, 4);
        slen += EFS_SESS_WIRE_LEN;
    }
    host_inode_forward(h, EFS_MSG_INODE_FLOCK, buf, slen,
                       EFS_MSG_INODE_FLOCK_REPLY, out, groups, ng);
}

static void host_fwd_append(struct efs_raft_host *h, efs_ino_t ino, uint64_t len,
                            const uint8_t *sess_uuid, uint32_t sess_epoch,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_append) + EFS_SESS_WIRE_LEN];
    struct efs_msg_inode_append *req = (struct efs_msg_inode_append *)buf;
    uint32_t slen = sizeof(*req);

    memset(buf, 0, sizeof(buf));
    req->ino = ino;
    req->len = len;
    if (sess_uuid) {
        memcpy(buf + slen, sess_uuid, EFS_OPID_UUID_LEN);
        memcpy(buf + slen + EFS_OPID_UUID_LEN, &sess_epoch, 4);
        slen += EFS_SESS_WIRE_LEN;
    }
    host_inode_forward(h, EFS_MSG_INODE_APPEND, buf, slen,
                       EFS_MSG_INODE_APPEND_REPLY, out, groups, ng);
}

static void host_fwd_unlink(struct efs_raft_host *h, efs_ino_t parent,
                            const char *name, int is_dir,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_unlink req;

    memset(&req, 0, sizeof(req));
    req.parent = parent;
    strncpy(req.name, name, EFS_MAX_NAME - 1);
    req.is_dir = is_dir ? 1 : 0;
    host_inode_forward(h, EFS_MSG_INODE_UNLINK, &req, sizeof(req),
                       EFS_MSG_INODE_UNLINK_REPLY, out, groups, ng);
}

static void host_fwd_link(struct efs_raft_host *h, efs_ino_t src_ino,
                          efs_ino_t new_parent, const char *new_name,
                          struct efs_msg_inode_reply *out,
                          const uint8_t *groups, int ng)
{
    struct efs_msg_inode_link req;

    memset(&req, 0, sizeof(req));
    req.src_ino = src_ino;
    req.new_parent = new_parent;
    strncpy(req.new_name, new_name, EFS_MAX_NAME - 1);
    host_inode_forward(h, EFS_MSG_INODE_LINK, &req, sizeof(req),
                       EFS_MSG_INODE_LINK_REPLY, out, groups, ng);
}

static void host_fwd_rename(struct efs_raft_host *h, efs_ino_t old_parent,
                            const char *old_name, efs_ino_t new_parent,
                            const char *new_name,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_rename_at req;

    memset(&req, 0, sizeof(req));
    req.old_parent = old_parent;
    strncpy(req.old_name, old_name, EFS_MAX_NAME - 1);
    req.new_parent = new_parent;
    strncpy(req.new_name, new_name, EFS_MAX_NAME - 1);
    host_inode_forward(h, EFS_MSG_INODE_RENAME_AT, &req, sizeof(req),
                       EFS_MSG_INODE_RENAME_AT_REPLY, out, groups, ng);
}

static void host_fwd_lookup_path(struct efs_raft_host *h, efs_ino_t start,
                                 const char *path,
                                 struct efs_msg_inode_lookup_path_reply *out,
                                 const uint8_t *groups, int ng)
{
    struct efs_msg_inode_lookup_path req;
    int tries, rid, skip = -1;

    memset(&req, 0, sizeof(req));
    req.start = start;
    if (path)
        strncpy(req.path, path, sizeof(req.path) - 1);
    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_NOT_PRIMARY;
    for (tries = 0; tries < h->n; tries++) {
        rid = host_pick_peer(h, groups, ng, skip);
        if (rid < 0)
            return;
        if (host_inode_rpc_peer(h, rid, EFS_MSG_INODE_LOOKUP_PATH, &req,
                                sizeof(req), EFS_MSG_INODE_LOOKUP_PATH_REPLY,
                                out, sizeof(*out)) != 0) {
            skip = rid;
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            continue;
        }
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            return;
        skip = rid;
    }
}

static void host_fwd_readdir(struct efs_raft_host *h, efs_ino_t parent,
                             uint32_t max_ents, uint32_t after_src,
                             const char *after_name,
                             struct efs_msg_inode_readdir_reply *out,
                             const uint8_t *groups, int ng)
{
    struct efs_msg_inode_readdir req;
    int tries, rid, skip = -1;

    memset(&req, 0, sizeof(req));
    req.parent = parent;
    req.max_ents = max_ents;
    req.after_src = after_src;
    if (after_name)
        strncpy(req.after_name, after_name, EFS_MAX_NAME - 1);
    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_NOT_PRIMARY;
    for (tries = 0; tries < h->n; tries++) {
        rid = host_pick_peer(h, groups, ng, skip);
        if (rid < 0)
            return;
        if (host_inode_rpc_peer(h, rid, EFS_MSG_INODE_READDIR, &req,
                                sizeof(req), EFS_MSG_INODE_READDIR_REPLY,
                                out, sizeof(*out)) != 0) {
            skip = rid;
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            continue;
        }
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            return;
        skip = rid;
    }
}

static void host_fwd_setattr(struct efs_raft_host *h, efs_ino_t ino,
                             uint32_t mask, uint32_t mode, uint32_t uid,
                             uint32_t gid, uint64_t size, uint64_t mtime,
                             uint32_t mtime_nsec, uint64_t atime,
                             struct efs_msg_inode_reply *out,
                             const uint8_t *groups, int ng)
{
    struct efs_msg_inode_setattr req;

    memset(&req, 0, sizeof(req));
    req.ino = ino;
    req.mask = mask;
    req.mode = mode;
    req.uid = uid;
    req.gid = gid;
    req.size = size;
    req.mtime = mtime;
    req.mtime_nsec = mtime_nsec;
    req.atime = atime;
    host_inode_forward(h, EFS_MSG_INODE_SETATTR, &req, sizeof(req),
                       EFS_MSG_INODE_SETATTR_REPLY, out, groups, ng);
}

static void host_fwd_getchunks(struct efs_raft_host *h, efs_ino_t ino,
                               uint32_t start, uint32_t max,
                               struct efs_msg_inode_getchunks_reply *out,
                               const uint8_t *groups, int ng)
{
    struct efs_msg_inode_getchunks req;
    int tries, rid, skip = -1;

    memset(&req, 0, sizeof(req));
    req.ino = ino;
    req.start = start;
    req.max = max;
    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_NOT_PRIMARY;
    for (tries = 0; tries < h->n; tries++) {
        rid = host_pick_peer(h, groups, ng, skip);
        if (rid < 0)
            return;
        if (host_inode_rpc_peer(h, rid, EFS_MSG_INODE_GETCHUNKS, &req,
                                sizeof(req), EFS_MSG_INODE_GETCHUNKS_REPLY,
                                out, sizeof(*out)) != 0) {
            skip = rid;
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            continue;
        }
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            return;
        skip = rid;
    }
}

/* REPORT_CHUNKS is variable-length (header + chunk recs + inode recs), so
 * the forward rebuilds the wire buffer from the parsed arrays. The raft
 * path is single-export; export_id 0 is what the receiver's handler keys
 * on there too. */
static void host_fwd_report(struct efs_raft_host *h,
                            const struct efs_chunk_rec *recs, uint32_t count,
                            const struct efs_ino_size_rec *irecs,
                            uint32_t ino_count,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_report_chunks *hdr;
    uint8_t *buf;
    size_t blen;
    int tries, rid, skip = -1;

    blen = sizeof(*hdr) + (size_t)count * sizeof(*recs) +
           (size_t)ino_count * sizeof(*irecs);
    buf = malloc(blen);
    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_NOT_PRIMARY;
    if (!buf)
        return;
    hdr = (struct efs_msg_report_chunks *)buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->count = count;
    hdr->ino_count = ino_count;
    memcpy(buf + sizeof(*hdr), recs, (size_t)count * sizeof(*recs));
    if (ino_count && irecs)
        memcpy(buf + sizeof(*hdr) + (size_t)count * sizeof(*recs), irecs,
               (size_t)ino_count * sizeof(*irecs));
    for (tries = 0; tries < h->n; tries++) {
        rid = host_pick_peer(h, groups, ng, skip);
        if (rid < 0)
            break;
        if (host_inode_rpc_peer(h, rid, EFS_MSG_REPORT_CHUNKS, buf,
                                (uint32_t)blen, EFS_MSG_REPORT_CHUNKS_REPLY,
                                out, sizeof(*out)) != 0) {
            skip = rid;
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            continue;
        }
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            break;
        skip = rid;
    }
    free(buf);
}

static int pack_create_cmd(uint8_t *out, uint32_t *len, efs_ino_t parent,
                           uint32_t mode, const char *name,
                           const struct efs_meta_attrs *at)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl == 0 || nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = HOST_CREATE_NAME_OFF + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_CREATE;
    out[1] = 0; /* no op-id yet */
    wr64be(out + 2, parent);
    wr32be(out + 10, mode);
    wr32be(out + 14, at->uid);
    wr32be(out + 18, at->gid);
    wr64be(out + 22, at->now);
    out[30] = (uint8_t)nl;
    memcpy(out + HOST_CREATE_NAME_OFF, name, nl);
    p = out + HOST_CREATE_NAME_OFF + nl;
    memset(p, 0, EFS_OPID_UUID_LEN + 4);
    *len = n;
    return EFS_OK;
}

/* Same encoding as sim pack_unlink. Session bytes are zero (not hosted). */
static int pack_unlink_cmd(uint8_t *out, uint32_t *len, efs_ino_t parent,
                           uint64_t now, const char *name)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl == 0 || nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = 18 + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_UNLINK;
    wr64be(out + 1, parent);
    wr64be(out + 9, now);
    out[17] = (uint8_t)nl;
    memcpy(out + 18, name, nl);
    p = out + 18 + nl;
    memset(p, 0, EFS_OPID_UUID_LEN + 4);
    *len = n;
    return EFS_OK;
}

/* Same encoding as sim_raft_setattr. Session bytes are zero (not hosted). */
static int pack_setattr_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                            uint64_t now, const struct efs_meta_setattr *sa)
{
    if (!sa)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_SETATTR;
    wr64be(out + 1, ino);
    wr64be(out + 9, now);
    wr64be(out + 17, sa->expect_gen);
    wr32be(out + 25, sa->mask);
    wr32be(out + 29, sa->mode);
    wr32be(out + 33, sa->uid);
    wr32be(out + 37, sa->gid);
    memset(out + 41, 0, EFS_OPID_UUID_LEN + 4);
    *len = HOST_SETATTR_LEN;
    return EFS_OK;
}

/* Same encoding as sim_raft_utimens. Session bytes are zero (not hosted). */
static int pack_utimens_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                            uint64_t now, const struct efs_meta_utimens *u)
{
    if (!u)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_UTIMENS;
    wr64be(out + 1, ino);
    wr64be(out + 9, now);
    wr64be(out + 17, u->expect_gen);
    wr32be(out + 25, u->mask);
    wr64be(out + 29, u->mtime);
    wr64be(out + 37, u->atime);
    wr64be(out + 45, u->mtime_gen);
    memset(out + 53, 0, EFS_OPID_UUID_LEN + 4);
    *len = HOST_UTIMENS_LEN;
    return EFS_OK;
}

/* Same encoding as sim_raft_truncate. Unaligned sizes carry a tail CAS. */
static int pack_truncate_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                             uint64_t now, uint64_t expect_gen, uint64_t size,
                             const struct efs_meta_pub *tail,
                             uint64_t lane_mask, uint8_t tail_external)
{
    uint8_t *q;
    int i;

    out[0] = EFS_MD_CMD_TRUNCATE;
    wr64be(out + 1, ino);
    wr64be(out + 9, now);
    wr64be(out + 17, expect_gen);
    wr64be(out + 25, size);
    out[33] = tail ? 1 : 0;
    memset(out + 34, 0, EFS_OPID_UUID_LEN + 4);
    wr64be(out + 54, lane_mask);
    out[62] = tail_external ? HOST_TRUNC_F_TAIL_EXT : 0;
    *len = HOST_TRUNC_LEN;
    if (!tail)
        return EFS_OK;
    q = out + HOST_TRUNC_LEN;
    wr32be(q, tail->chunk_index);
    wr64be(q + 4, tail->candidate_gen);
    wr64be(q + 12, tail->expected_gen);
    wr32be(q + 20, tail->coding_profile_id);
    q += 24;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        wr32be(q, tail->ch.nodes[i]);
        q += 4;
        memcpy(q, tail->ch.checksums[i], EFS_HASH_SIZE);
        q += EFS_HASH_SIZE;
    }
    *len = HOST_TRUNC_LEN + HOST_TRUNC_TAIL;
    return EFS_OK;
}

static int pack_append_rsv_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                               uint64_t alen, const uint8_t *uuid,
                               uint32_t epoch)
{
    out[0] = EFS_MD_CMD_APPEND_RSV;
    wr64be(out + 1, ino);
    wr64be(out + 9, alen);
    memset(out + 17, 0, EFS_OPID_UUID_LEN + 4 + 8);
    if (uuid) {
        memcpy(out + 17, uuid, EFS_OPID_UUID_LEN);
        wr32be(out + 33, epoch);
        /* seq stays 0: production host has no op-id window yet. */
    }
    *len = HOST_APPEND_RSV_LEN;
    return EFS_OK;
}

static int pack_append_res_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                               uint64_t off, int outcome)
{
    out[0] = EFS_MD_CMD_APPEND_RES;
    wr64be(out + 1, ino);
    wr64be(out + 9, off);
    out[17] = (uint8_t)outcome;
    memset(out + 18, 0, EFS_OPID_UUID_LEN + 4);
    *len = HOST_APPEND_RES_LEN;
    return EFS_OK;
}

/* Stand-in identity when the RPC has no session suffix (pre-35b FUSE
 * and smoke). Real (uuid, epoch) come from the optional wire suffix. */
static void host_hold_uuid(uint64_t owner, uint8_t uuid[EFS_OPID_UUID_LEN])
{
    memset(uuid, 0, EFS_OPID_UUID_LEN);
    wr64be(uuid, owner);
}

static void host_sess_bytes(uint64_t owner, const uint8_t *sess_uuid,
                            uint32_t sess_epoch, uint8_t uuid[EFS_OPID_UUID_LEN],
                            uint32_t *epoch)
{
    if (sess_uuid) {
        memcpy(uuid, sess_uuid, EFS_OPID_UUID_LEN);
        *epoch = sess_epoch;
        return;
    }
    host_hold_uuid(owner, uuid);
    *epoch = 1;
}

/* Absent suffix skips accept (stand-in). Present suffix must already
 * be established on the inode shard (35a CREATE/REGISTER/ESTABLISH). */
static int host_sess_gate(struct efs_raft_host *h, efs_ino_t ino,
                          const uint8_t *sess_uuid, uint32_t sess_epoch)
{
    if (!sess_uuid)
        return EFS_OK;
    return efs_session_accept(h->kv, efs_kv_inode_shard(ino), sess_uuid,
                              sess_epoch);
}

static int pack_lease_cmd(uint8_t *out, uint32_t *len, int open, efs_ino_t ino,
                          uint64_t gen, const uint8_t uuid[EFS_OPID_UUID_LEN],
                          uint32_t epoch)
{
    out[0] = EFS_MD_CMD_SESSION;
    out[1] = open ? EFS_MD_SESS_LEASE_OPEN : EFS_MD_SESS_LEASE_CLOSE;
    memcpy(out + 2, uuid, EFS_OPID_UUID_LEN);
    wr32be(out + 18, epoch);
    wr64be(out + 22, ino);
    wr64be(out + 30, gen);
    *len = HOST_SESS_LEASE_LEN;
    return EFS_OK;
}

/* Same encoding as sim pack_lock. */
static int pack_lock_cmd(uint8_t *out, uint32_t *len, uint8_t kind,
                         const struct efs_lock_req *r)
{
    uint8_t *p;

    if (!r)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_LOCK;
    out[1] = kind;
    wr64be(out + 2, r->ino);
    wr64be(out + 10, r->generation);
    out[18] = r->domain;
    out[19] = r->type;
    wr64be(out + 20, r->start);
    wr64be(out + 28, r->end);
    out[36] = r->owner.kind;
    wr64be(out + 37, r->owner.id);
    p = out + 45;
    memcpy(p, r->owner.uuid, EFS_OPID_UUID_LEN);
    wr32be(p + EFS_OPID_UUID_LEN, r->owner.epoch);
    *len = HOST_LOCK_LEN;
    return EFS_OK;
}

static void fill_flock_req(struct efs_lock_req *r, efs_ino_t ino, uint64_t gen,
                           uint8_t type, uint64_t owner, uint8_t domain,
                           uint64_t start, uint64_t end,
                           const uint8_t *sess_uuid, uint32_t sess_epoch)
{
    memset(r, 0, sizeof(*r));
    r->ino = ino;
    r->generation = gen;
    r->domain = domain;
    r->type = type;
    r->start = start;
    r->end = end;
    /* Classic fcntl = process token; flock (and OFD fcntl) = OFD. */
    r->owner.kind = (domain == EFS_LOCK_FCNTL) ? EFS_LOCK_PROC : EFS_LOCK_OFD;
    r->owner.id = owner;
    host_sess_bytes(owner, sess_uuid, sess_epoch, r->owner.uuid,
                    &r->owner.epoch);
}

/* Same encoding as sim pack_publish. Session bytes are zero (not hosted). */
static int pack_publish_cmd(uint8_t *out, uint32_t *len, const struct efs_meta_pub *p)
{
    uint8_t *q;
    int i;

    if (!p)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_PUBLISH;
    wr64be(out + 1, p->ino);
    wr32be(out + 9, p->chunk_index);
    wr64be(out + 13, p->new_size);
    wr64be(out + 21, p->now);
    q = out + 29;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        wr32be(q, p->ch.nodes[i]);
        q += 4;
        memcpy(q, p->ch.checksums[i], EFS_HASH_SIZE);
        q += EFS_HASH_SIZE;
    }
    memset(q, 0, EFS_OPID_UUID_LEN + 4);
    q += EFS_OPID_UUID_LEN + 4;
    wr64be(q, p->candidate_gen);
    wr64be(q + 8, p->expected_gen);
    wr64be(q + 16, p->content_epoch);
    wr32be(q + 24, p->coding_profile_id);
    q += 28;
    wr64be(q, p->inode_gen);
    wr64be(q + 8, p->mtime_gen);
    q[16] = p->lane_local ? HOST_PUB_F_LANE_LOCAL : 0;
    *len = HOST_PUBLISH_LEN;
    return EFS_OK;
}

/* EFS_MD_CMD_ACTIVATE_LANE: [ino:8][lane:1] — meta_cmd.h. */
static int pack_activate_lane_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                                  uint8_t lane)
{
    out[0] = EFS_MD_CMD_ACTIVATE_LANE;
    wr64be(out + 1, ino);
    out[9] = lane;
    *len = HOST_ACTIVATE_LANE_LEN;
    return EFS_OK;
}

/* EFS_MD_CMD_LANE_FENCE: [ino:8][gen:8][lane:1][epoch:8][size:8]
 * [tail_ci:4][has_tail:1] — meta_cmd.h. */
static int pack_lane_fence_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                               uint64_t gen, uint8_t lane, uint64_t new_epoch,
                               uint64_t size, uint32_t tail_ci,
                               uint8_t has_tail)
{
    out[0] = EFS_MD_CMD_LANE_FENCE;
    wr64be(out + 1, ino);
    wr64be(out + 9, gen);
    out[17] = lane;
    wr64be(out + 18, new_epoch);
    wr64be(out + 26, size);
    wr32be(out + 34, tail_ci);
    out[38] = has_tail;
    *len = HOST_LANE_FENCE_LEN;
    return EFS_OK;
}

static uint32_t pack_prep(uint8_t *out, int kind, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, uint64_t expected, int op,
                          const uint8_t *val, uint32_t vlen)
{
    uint32_t n, i;

    out[0] = EFS_MD_CMD_PREPARE;
    out[1] = (uint8_t)kind;
    memcpy(out + 2, t->bytes, 16);
    out[18] = p->n;
    n = 19;
    for (i = 0; i < p->n; i++) {
        wr32be(out + n, p->shard[i]);
        n += 4;
    }
    /* klen is 2 bytes: a dentry key is 11 + name, and a 255-char name is
     * 266 bytes — a 1-byte field truncates and the intent lands under a
     * garbage key (the txn commits, the name is never visible). */
    out[n++] = (uint8_t)(klen >> 8);
    out[n++] = (uint8_t)klen;
    memcpy(out + n, key, klen);
    n += klen;
    wr64be(out + n, expected);
    out[n + 8] = (uint8_t)op;
    wr32be(out + n + 9, vlen);
    n += 13;
    if (vlen) {
        memcpy(out + n, val, vlen);
        n += vlen;
    }
    return n;
}

static void pack_decide(uint8_t *out, const struct efs_txid *t, uint32_t coord,
                        int dec)
{
    out[0] = EFS_MD_CMD_DECIDE;
    memcpy(out + 1, t->bytes, 16);
    wr32be(out + 17, coord);
    out[21] = (uint8_t)dec;
}

static void pack_resolve(uint8_t *out, const struct efs_txid *t, uint32_t shard,
                         int dec)
{
    out[0] = EFS_MD_CMD_RESOLVE;
    memcpy(out + 1, t->bytes, 16);
    wr32be(out + 17, shard);
    out[21] = (uint8_t)dec;
}

static void pack_drop(uint8_t *out, const struct efs_txid *t, uint32_t shard)
{
    out[0] = EFS_MD_CMD_DROP;
    memcpy(out + 1, t->bytes, 16);
    wr32be(out + 17, shard);
}

static void fill_txid(struct efs_raft_host *h, struct efs_txid *t)
{
    uint64_t a = now_ns();
    uint64_t b = h->boot_id ^ h->salt ^ ((uint64_t)h->raft_id << 32);

    memcpy(t->bytes, &a, 8);
    memcpy(t->bytes + 8, &b, 8);
}

static int host_txn_coord(void *user, const struct efs_txid *t,
                          uint32_t coord_shard, int *dec)
{
    struct efs_raft_host *h = user;
    uint8_t group;
    int hint = -1;
    int rc;

    if (!h || !h->kv || !t || !dec)
        return EFS_ERR_IO;
    group = efs_raft_shard_group(coord_shard);
    rc = host_read_index(h, group, &hint);
    if (rc != EFS_OK)
        return EFS_ERR_IO; /* I9: cannot establish authority, never absence */
    return efs_txn_decision_get(h->kv, coord_shard, t, dec);
}

static void stat_to_inode(const struct efs_meta_stat *st, struct efs_inode *ino)
{
    memset(ino, 0, sizeof(*ino));
    ino->ino = st->ino;
    ino->mode = st->mode;
    ino->nlink = st->nlink;
    ino->uid = (uid_t)st->uid;
    ino->gid = (gid_t)st->gid;
    ino->size = st->size;
    ino->mtime = st->mtime / 1000000000ull;
    ino->mtime_nsec = (uint32_t)(st->mtime % 1000000000ull);
    ino->ctime = st->ctime / 1000000000ull;
    ino->ctime_nsec = (uint32_t)(st->ctime % 1000000000ull);
    ino->atime = st->atime / 1000000000ull;
    ino->atime_nsec = (uint32_t)(st->atime % 1000000000ull);
}

static uint8_t rc_to_inode_status(int rc)
{
    if (rc == EFS_OK)
        return EFS_INODE_RPC_OK;
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_INODE_RPC_NOT_FOUND;
    if (rc == EFS_ERR_NOT_PRIMARY)
        return EFS_INODE_RPC_NOT_PRIMARY;
    if (rc == EFS_ERR_BUSY)
        return EFS_INODE_RPC_BUSY;
    if (rc == EFS_ERR_AGAIN)
        return EFS_INODE_RPC_BUSY;
    if (rc == EFS_ERR_NOLCK)
        return EFS_INODE_RPC_BUSY;
    if (rc == EFS_ERR_STALE)
        return EFS_INODE_RPC_BUSY;
    if (rc == EFS_ERR_INVAL)
        return EFS_INODE_RPC_INVAL;
    if (rc == EFS_ERR_EXIST)
        return EFS_INODE_RPC_EXIST;
    if (rc == EFS_ERR_NAMETOOLONG)
        return EFS_INODE_RPC_INVAL;
    if (rc == EFS_ERR_NOT_EMPTY)
        return EFS_INODE_RPC_NOT_EMPTY;
    return EFS_INODE_RPC_ERROR;
}

static void set_inode_rc(struct efs_msg_inode_reply *out, int rc, int leader_hint)
{
    out->status = rc_to_inode_status(rc);
    out->primary_id = (leader_hint >= 0) ? (efs_node_id_t)(leader_hint + 1) : 0;
}

static int host_read_inode_lanes(struct efs_raft_host *h, efs_ino_t ino,
                                 int *leader_hint)
{
    struct efs_meta_row row;
    uint32_t ish;
    uint8_t ig;
    uint64_t bits;
    uint32_t i;
    int rc;

    ish = efs_kv_inode_shard(ino);
    ig = efs_raft_shard_group(ish);
    rc = host_read_index(h, ig, leader_hint);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
               ? row.used_shards
               : row.active_lanes;
    for (i = 0; i < EFS_META_LANES; i++) {
        uint32_t lsh;
        uint8_t lg;
        int hint = -1;

        if ((bits & (1ULL << i)) == 0)
            continue;
        lsh = efs_kv_lane_shard(ino, (uint8_t)i);
        lg = efs_raft_shard_group(lsh);
        if (lg == ig)
            continue;
        rc = host_read_index(h, lg, &hint);
        if (rc != EFS_OK) {
            *leader_hint = hint;
            return rc;
        }
    }
    return EFS_OK;
}

static void drain_inbox(struct efs_raft_host *h)
{
    struct host_inbox_item local[HOST_INBOX_MAX];
    int n, i;

    pthread_mutex_lock(&h->inbox_mu);
    n = h->inbox_n;
    memcpy(local, h->inbox, (size_t)n * sizeof(local[0]));
    h->inbox_n = 0;
    pthread_mutex_unlock(&h->inbox_mu);

    for (i = 0; i < n; i++) {
        struct efs_raft_msg msg;
        uint8_t *cmd = NULL;
        uint32_t cmd_cap = 0;
        struct efs_raft *r;
        int rc;

        /* Batched AE: total cmd bytes = len - HDR - 12*nentries <= len - HDR.
         * Allocate the upper bound (the per-entry headers are slack). */
        if (local[i].len > EFS_WIRE_RAFT_HDR_LEN)
            cmd_cap = local[i].len - EFS_WIRE_RAFT_HDR_LEN;
        if (cmd_cap) {
            cmd = malloc(cmd_cap);
            if (!cmd) {
                free(local[i].buf);
                continue;
            }
        }
        rc = efs_wire_raft_decode(local[i].buf, local[i].len, &msg, cmd,
                                  cmd_cap);
        free(local[i].buf);
        if (rc != EFS_OK) {
            free(cmd);
            continue;
        }
        r = group_raft(h, msg.group);
        if (r)
            (void)efs_raft_recv(r, &msg);
        free(cmd);
    }
}

/* Persist last_applied without compacting the log. Restart restores it so
 * CREATE is not re-applied onto a KV that already ran a later rename. */
static void applied_path(struct efs_raft_host *h, uint8_t group,
                         char *path, size_t cap)
{
    snprintf(path, cap, "%s/mdraft/applied.%u", h->s->storage_path, group);
}

static int persist_applied(struct efs_raft_host *h, int gi)
{
    uint64_t idx;
    char path[EFS_MAX_PATH], tmp[EFS_MAX_PATH];
    uint8_t buf[8];
    int fd, n, rc;

    if (!h->g[gi].hosted || !h->g[gi].r)
        return EFS_OK;
    idx = efs_raft_applied(h->g[gi].r);
    if (idx == 0 || idx == h->g[gi].applied_saved)
        return EFS_OK;
    applied_path(h, h->g[gi].group, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    wr64be(buf, idx);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return EFS_ERR_IO;
    n = (int)write(fd, buf, 8);
    rc = fsync(fd);
    close(fd);
    if (n != 8 || rc != 0) {
        unlink(tmp);
        return EFS_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return EFS_ERR_IO;
    }
    h->g[gi].applied_saved = idx;
    return EFS_OK;
}

static int load_applied(struct efs_raft_host *h, int gi, uint64_t *idx)
{
    char path[EFS_MAX_PATH];
    uint8_t buf[8];
    int fd, n;

    *idx = 0;
    applied_path(h, h->g[gi].group, path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return EFS_OK;
    n = (int)read(fd, buf, 8);
    close(fd);
    if (n != 8)
        return EFS_OK;
    *idx = rd64be(buf);
    return EFS_OK;
}

/* Wake the pump from any thread. Never blocks, never takes h->mu — safe
 * from the network handler no matter what raft core work the pump (or a
 * proposer) is doing under h->mu. */
static void host_pump_kick(struct efs_raft_host *h)
{
    uint64_t one = 1;
    if (h->pump_efd >= 0)
        (void)write(h->pump_efd, &one, sizeof(one)); /* EAGAIN: already lit */
}

/* EFS_RAFT_OBS: log every term/role change (election churn has no other
 * footprint) and dump outbox/pump stats every ~5 s when anything moved. */
static void host_obs_dump(struct efs_raft_host *h, int force)
{
    uint64_t now_ms = now_us_() / 1000ull;
    int i, any = 0;
    static const char *roles[] = { "FOLLOWER", "CANDIDATE", "LEADER", "?" };

    for (i = 0; i < HOST_NGROUPS; i++) {
        struct host_group *g = &h->g[i];
        uint64_t t;
        int ro;
        if (!g->hosted || !g->r)
            continue;
        t = efs_raft_term(g->r);
        ro = efs_raft_role(g->r);
        if (ro < 0 || ro > 2)
            ro = 3;
        if (t != g->obs_term || ro != g->obs_role) {
            fprintf(stderr,
                    "raft-obs: g%u term %llu->%llu role %s->%s leader=%d "
                    "commit=%llu applied=%llu t=%llums\n",
                    g->group,
                    (unsigned long long)g->obs_term, (unsigned long long)t,
                    roles[g->obs_role >= 0 && g->obs_role <= 2 ? g->obs_role : 3],
                    roles[ro], efs_raft_leader(g->r),
                    (unsigned long long)efs_raft_commit(g->r),
                    (unsigned long long)efs_raft_applied(g->r),
                    (unsigned long long)now_ms);
            g->obs_term = t;
            g->obs_role = ro;
        }
    }
    if (!force && now_ms - h->obs_last_dump_ms < 5000)
        return;
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        struct host_outbox *tx = &h->tx[i];
        if (!tx->st_enq && !tx->st_drop)
            continue;
        any = 1;
        fprintf(stderr,
                "raft-obs: tx->%d enq=%llu drop=%llu sent=%llu fail=%llu "
                "hi=%u rtt_avg=%lluus get_avg=%lluus get_max=%llums\n",
                tx->peer,
                (unsigned long long)tx->st_enq,
                (unsigned long long)tx->st_drop,
                (unsigned long long)tx->st_sent,
                (unsigned long long)tx->st_fail,
                tx->st_hi,
                tx->st_sent ? (unsigned long long)(tx->st_rtt_us / tx->st_sent)
                            : 0ull,
                tx->st_sent + tx->st_fail
                    ? (unsigned long long)(tx->st_get_us /
                                           (tx->st_sent + tx->st_fail))
                    : 0ull,
                (unsigned long long)(tx->st_get_max_us / 1000ull));
    }
    if (any || h->obs_wait_timeouts || h->obs_pump_hold_max_us ||
        h->obs_arc_miss) {
        fprintf(stderr,
                "raft-obs: wait_timeouts=%llu pump_hold_max=%lluus "
                "lock_wait_max=%lluus drain_max=%lluus tick_max=%lluus "
                "apply_max=%lluus persist_max=%lluus applies_in_worst=%llu "
                "arc_miss=%llu\n",
                (unsigned long long)h->obs_wait_timeouts,
                (unsigned long long)h->obs_pump_hold_max_us,
                (unsigned long long)h->obs_wait_max_us,
                (unsigned long long)h->obs_drain_max_us,
                (unsigned long long)h->obs_tick_max_us,
                (unsigned long long)h->obs_apply_max_us,
                (unsigned long long)h->obs_persist_max_us,
                (unsigned long long)h->obs_apply_max_cnt,
                (unsigned long long)h->obs_arc_miss);
        h->obs_pump_hold_max_us = 0;
        h->obs_wait_max_us = 0;
        h->obs_drain_max_us = 0;
        h->obs_tick_max_us = 0;
        h->obs_apply_max_us = 0;
        h->obs_persist_max_us = 0;
        h->obs_apply_max_cnt = 0;
    }
    if (any)
        h->obs_last_dump_ms = now_ms;
}

static void *host_pump(void *arg)
{
    struct efs_raft_host *h = arg;
    uint64_t last_tick_us = 0;

    while (h->running) {
        struct pollfd pfd;
        uint64_t sink;
        uint64_t c0 = 0;
        int i;
        int obs = raft_obs_on();
        uint64_t t_drain = 0, t_tick = 0, t_persist = 0, t_wait = 0;
        if (obs) {
            c0 = now_us_();
            h->obs_apply_cnt = 0;
            h->obs_apply_cycle_us = 0;
        }
        pthread_mutex_lock(&h->mu);
        if (obs) {
            t_wait = now_us_() - c0;
            c0 = now_us_();
        }
        drain_inbox(h);
        if (obs) {
            t_drain = now_us_() - c0;
            c0 = now_us_();
        }
        /* Tick on WALL time, never per loop iteration: the eventfd below
         * wakes the loop on every inbox message/propose, so under load the
         * loop spins as fast as the drain runs. raft.c counts one tick as
         * HOST_TICK_US of election/heartbeat time; a tick per spinning
         * iteration fires the 100-200-tick election deadline in
         * milliseconds and storms the term (observed: 3 re-campaigns in
         * 35 ms, term +8 in 75 ms). Late ticks are safe (they only delay
         * timeouts); early ones are not, so never catch up missed ticks. */
        {
            uint64_t now = now_us_();
            if (now - last_tick_us >= HOST_TICK_US) {
                for (i = 0; i < HOST_NGROUPS; i++) {
                    if (h->g[i].hosted && h->g[i].r)
                        (void)efs_raft_tick(h->g[i].r);
                }
                last_tick_us = now;
            }
        }
        if (obs) {
            t_tick = now_us_() - c0;
            c0 = now_us_();
        }
        for (i = 0; i < HOST_NGROUPS; i++)
            (void)persist_applied(h, i);
        if (obs)
            t_persist = now_us_() - c0;
        /* Applies above may have advanced commit/applied: wake every waiter
         * (host_wait_applied / host_read_index) without a poll interval. */
        pthread_cond_broadcast(&h->applied_cv);
        if (obs) {
            uint64_t held;
            if (t_wait > h->obs_wait_max_us)
                h->obs_wait_max_us = t_wait;
            if (t_drain > h->obs_drain_max_us)
                h->obs_drain_max_us = t_drain;
            if (t_tick > h->obs_tick_max_us)
                h->obs_tick_max_us = t_tick;
            if (t_persist > h->obs_persist_max_us)
                h->obs_persist_max_us = t_persist;
            if (h->obs_apply_cycle_us > h->obs_apply_max_us)
                h->obs_apply_max_us = h->obs_apply_cycle_us;
            if (h->obs_apply_cnt > h->obs_apply_max_cnt)
                h->obs_apply_max_cnt = h->obs_apply_cnt;
            held = t_drain + t_tick + t_persist;
            if (held > h->obs_pump_hold_max_us)
                h->obs_pump_hold_max_us = held;
            host_obs_dump(h, 0);
        }
        pthread_mutex_unlock(&h->mu);
        /* Sleep until the next raft timer tick OR an event (inbox message,
         * local propose, shutdown) — whichever comes first. Timer cadence is
         * unchanged; events just remove the up-to-5 ms wait. h->mu is NOT
         * held here, so proposers and the inbox never queue behind it. */
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = h->pump_efd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, HOST_TICK_US / 1000) > 0 && (pfd.revents & POLLIN))
            (void)read(h->pump_efd, &sink, sizeof(sink));
    }
    return NULL;
}

static uint64_t make_boot_id(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)getpid() << 32) ^
           ((uint64_t)ts.tv_sec << 16) ^ (uint64_t)ts.tv_nsec;
}

/* Boot ids fence stale incarnations (raft.c peer_boot): a peer drops any
 * message whose boot is LOWER than the last one it saw from us. pid^time is
 * NOT monotonic across restarts, so an unfenced restart could pick a lower
 * boot and be silently fenced forever (replies dropped, replication wedges).
 * Persist the last used boot in <mdraft>/boot.bin and always start at
 * max(fresh, saved+1), writing the chosen value back before use. */
static int host_boot_bump(const char *dir, uint64_t *boot_io)
{
    char path[EFS_MAX_PATH];
    uint64_t saved = 0, next;
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), "%s/boot.bin", dir);
    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return EFS_ERR_IO;
    n = read(fd, &saved, sizeof(saved));
    if (n < 0) {
        close(fd);
        return EFS_ERR_IO;
    }
    if (n < (ssize_t)sizeof(saved))
        saved = 0; /* fresh file */
    next = *boot_io;
    if (saved >= next)
        next = saved + 1;
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        write(fd, &next, sizeof(next)) != (ssize_t)sizeof(next) ||
        fsync(fd) != 0) {
        close(fd);
        return EFS_ERR_IO;
    }
    close(fd);
    *boot_io = next;
    return EFS_OK;
}

static uint64_t make_salt(uint64_t boot)
{
    uint64_t s = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&s, 1, sizeof(s), f) != sizeof(s))
            s = 0;
        fclose(f);
    }
    if (s == 0)
        s = boot ^ 0x9e3779b97f4a7c15ULL;
    return s;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int attach_group(struct efs_raft_host *h, int gi, uint8_t group)
{
    struct efs_raft_cfg cfg;
    struct efs_raft_store *st;
    uint32_t voters = group_voters(group, h->n);

    h->g[gi].group = group;
    h->g[gi].voters = voters;
    h->g[gi].host = h;
    h->g[gi].hosted = (uint8_t)hosts_group(h->raft_id, voters);
    h->g[gi].r = NULL;
    memset(h->g[gi].arc_idx, 0, sizeof(h->g[gi].arc_idx));
    memset(h->g[gi].arc_rc, 0, sizeof(h->g[gi].arc_rc));
    if (!h->g[gi].hosted)
        return EFS_OK;
    st = efs_raft_disk_group(h->disk, group);
    if (!st)
        return EFS_ERR_IO;
    memset(&cfg, 0, sizeof(cfg));
    cfg.id = h->raft_id;
    cfg.n = h->n;
    cfg.voters = voters;
    cfg.election_ticks = HOST_ELECT_BASE;
    cfg.heartbeat_ticks = HOST_HB_TICKS;
    cfg.boot_id = h->boot_id;
    /* Entropy-seeded (h->salt reads /dev/urandom) per-(node,group) so the
     * randomized election timeout is actually de-synchronized in production. */
    cfg.rng_seed = h->salt ^ ((uint64_t)(h->raft_id + 1) << 32) ^
                   ((uint64_t)(group + 1) * 0x9e3779b97f4a7c15ULL);
    cfg.group = group;
    cfg.store = st;
    cfg.store_ctx = st;
    cfg.send = host_send;
    cfg.net = h;
    cfg.apply = host_apply;
    cfg.app = &h->g[gi]; /* per-group: host_apply records into g->arc_* */
    h->g[gi].r = efs_raft_new(&cfg);
    if (!h->g[gi].r)
        return EFS_ERR_NOMEM;
    {
        uint64_t applied = 0;
        if (load_applied(h, gi, &applied) == EFS_OK && applied > 0) {
            (void)efs_raft_restore_applied(h->g[gi].r, applied);
            h->g[gi].applied_saved = efs_raft_applied(h->g[gi].r);
        }
    }
    return EFS_OK;
}

/* ---- background GC reaper (step-11 follow-up, spec L7) ----
 *
 * Unlink/reclaim leave a REAP marker (kind 22) and truncate/publish-CAS
 * leave GC records (kind 21) in the KV, all anchored on shard 1 (odd
 * shards, group 0) or shard 2 (even shards, group 2) so ONE prefix scan
 * per group finds every pending record. This thread runs two passes on
 * each group this node leads:
 *
 *   REAP pass: per marker, propose LANE_SWEEP for every lane in the
 *   marker's active_lanes bitmap to the lane's own group (deletes the
 *   lane's chunk keys, emitting a GC record per chunk), then REAP_DONE
 *   to the inode's group (clears append cursor/reservations + marker).
 *
 *   GC pass: per record, delete each un-acked fragment — local
 *   del_if_sum when the record's nodes[i] is this node, else a
 *   GC_FRAGMENT RPC to that node — then propose the acks as one GC_ACK
 *   entry. The apply side deletes the record once all fragments ack.
 *
 * Everything is idempotent and re-driven every pass, so a lost proposal,
 * a leadership change or a crash just means the next pass retries.
 * Proposals go through host_propose_wait with read_mu HELD (its forward
 * path drops/re-takes read_mu internally — the established handler
 * pattern); scans and fragment I/O run outside every host lock. */

#define GC_SCAN_MAX   32 /* records collected per GC pass */
#define REAP_SCAN_MAX 32 /* markers collected per REAP pass */
#define GC_ACK_MAX    16 /* items per GC_ACK entry (3+22*16=355 <= 512) */
#define GC_LOOP_MS    1000

#define GC_KEY_LEN   24u /* [anchor:2][GC:1][ino:8][gen:8][lane:1][ci:4] */
#define REAP_KEY_LEN 11u /* [anchor:2][REAP:1][ino:8] */

struct gc_scan_ctx {
    int n;
    int full;
    uint8_t keys[GC_SCAN_MAX][GC_KEY_LEN];
    uint8_t vals[GC_SCAN_MAX][EFS_META_GC_VAL];
};

static int gc_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                      const uint8_t *val, uint32_t vlen)
{
    struct gc_scan_ctx *c = user;

    if (c->n >= GC_SCAN_MAX) {
        c->full = 1;
        return 1;
    }
    if (klen == GC_KEY_LEN && vlen >= EFS_META_GC_VAL) {
        memcpy(c->keys[c->n], key, GC_KEY_LEN);
        memcpy(c->vals[c->n], val, EFS_META_GC_VAL);
        c->n++;
    }
    return 0;
}

struct reap_scan_ctx {
    int n;
    int full;
    efs_ino_t ino[REAP_SCAN_MAX];
    uint64_t gen[REAP_SCAN_MAX];
    uint64_t lanes[REAP_SCAN_MAX];
};

static int reap_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                        const uint8_t *val, uint32_t vlen)
{
    struct reap_scan_ctx *c = user;

    if (c->n >= REAP_SCAN_MAX) {
        c->full = 1;
        return 1;
    }
    if (klen == REAP_KEY_LEN &&
        efs_meta_unpack_reap(val, vlen, &c->gen[c->n],
                             &c->lanes[c->n]) == EFS_OK) {
        c->ino[c->n] = rd64be(key + 3);
        c->n++;
    }
    return 0;
}

/* The export the reaper's fragment deletes/RPCs name. Raft mode has
 * exactly one export in practice (the mkfs shell); the GC record carries
 * no export id, so multi-export GC would need the id in the record — a
 * documented v1 limit. Returns with an inflight ref held (caller puts). */
static struct efs_export *host_gc_export(struct efs_raft_host *h)
{
    struct efs_export *ex = NULL;

    pthread_mutex_lock(&h->s->lock);
    if (h->s->export_count > 0)
        ex = server_export_acquire_locked(h->s, h->s->exports[0].id);
    pthread_mutex_unlock(&h->s->lock);
    return ex;
}

/* Delete one fragment on THIS node, checksum-conditional. EFS_OK when the
 * dead bytes are gone afterwards (deleted / absent / slot reused). */
static int host_gc_local_del(struct efs_raft_host *h, struct efs_export *ex,
                             efs_ino_t ino, uint32_t ci, uint32_t fi,
                             const uint8_t *sum)
{
    struct efs_store st;
    struct efs_nvme_store nctx;
    struct efs_frag_id fid;
    int rc;

    memset(&fid, 0, sizeof(fid));
    fid.export_id = ex->id;
    fid.ino = ino;
    fid.chunk_index = ci;
    fid.fragment_index = fi;
    efs_store_nvme_bind(&st, &nctx, h->s, ex);
    rc = efs_store_del_if_sum(&st, &fid, sum);
    if (rc == EFS_ERR_EXIST)
        rc = EFS_OK; /* slot reused: the dead generation is already gone */
    return rc;
}

/* Ask the owning node to delete one fragment. EFS_OK = gone/ackable. */
static int host_gc_remote_del(struct efs_raft_host *h, efs_node_id_t node,
                              efs_export_id_t export_id, efs_ino_t ino,
                              uint32_t ci, uint32_t fi, const uint8_t *sum)
{
    char host[64];
    uint16_t port = 0;
    struct efs_conn *pc;
    struct efs_msg_gc_fragment req;
    struct efs_msg_gc_fragment_reply *rep;
    uint8_t rtype = 0;
    void *reply = NULL;
    uint32_t rlen = 0;
    int rc = EFS_ERR_IO;

    /* nodes[] in a chunk/GC value are 1-based server node ids; peer_addr
     * keys off the 0-based raft id. */
    if (node == 0 || (int)node - 1 == h->raft_id ||
        peer_addr(h, (int)node - 1, host, sizeof(host), &port) != 0)
        return EFS_ERR_IO;
    pc = server_peer_conn_get(host, port);
    if (!pc)
        return EFS_ERR_IO;
    /* A dead peer must not sit on the pool's long default timeout: the
     * reaper would otherwise serialize minutes per dead node per pass. */
    if (pc->kind == EFS_CONN_TCP && pc->fd >= 0) {
        efs_set_recv_timeout(pc->fd, HOST_SEND_IO_MS);
        efs_set_send_timeout(pc->fd, HOST_SEND_IO_MS);
    }
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.chunk_index = ci;
    req.fragment_index = fi;
    memcpy(req.checksum, sum, EFS_HASH_SIZE);
    if (efs_conn_send_msg(pc, EFS_MSG_GC_FRAGMENT, &req, sizeof(req)) != 0)
        goto out;
    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) == 0 &&
        rtype == EFS_MSG_GC_FRAGMENT_REPLY &&
        rlen >= sizeof(struct efs_msg_gc_fragment_reply)) {
        rep = reply;
        if (rep->status == 0)
            rc = EFS_OK;
    }
out:
    free(reply);
    if (rc == EFS_OK) {
        if (pc->kind == EFS_CONN_TCP && pc->fd >= 0) {
            efs_set_recv_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
            efs_set_send_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
        }
        server_peer_conn_release(host, port, pc);
    } else {
        server_peer_conn_drop(host, port, pc);
    }
    return rc;
}

/* Propose one GC command and wait for it to apply. read_mu is taken per
 * proposal: host_propose's forward path drops/re-takes it internally, so
 * the caller must hold it — but the slow fragment I/O runs without it. */
static int host_gc_propose(struct efs_raft_host *h, uint8_t group,
                           const uint8_t *cmd, uint32_t clen)
{
    int rc;

    pthread_mutex_lock(&h->read_mu);
    rc = host_propose_wait(h, group, cmd, clen, NULL);
    pthread_mutex_unlock(&h->read_mu);
    return rc;
}

/* One GC record: attempt every un-acked fragment, appending an ack item
 * for each one now gone. At most GC_ACK_MAX acks are collected per pass
 * (one entry's worth); the rest are retried next pass. */
static void host_gc_record(struct efs_raft_host *h, struct efs_export *ex,
                           const uint8_t *key, const uint8_t *val,
                           struct efs_gc_ack_item *acks, int *nack)
{
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    uint8_t ack_bits = 0;
    efs_ino_t ino;
    uint64_t gen;
    uint32_t ci;
    uint8_t lane;
    int fi;

    if (efs_meta_unpack_gc(val, EFS_META_GC_VAL, nodes, &ack_bits,
                           sums) != EFS_OK)
        return;
    ino = rd64be(key + 3);
    gen = rd64be(key + 11);
    lane = key[19];
    ci = rd32be(key + 20);
    for (fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        int rc;

        if (ack_bits & (1u << fi))
            continue;
        if (*nack >= GC_ACK_MAX || !h->gc_running)
            return;
        if (nodes[fi] == h->s->id)
            rc = host_gc_local_del(h, ex, ino, ci, (uint32_t)fi, sums[fi]);
        else
            rc = host_gc_remote_del(h, nodes[fi], ex->id, ino, ci,
                                    (uint32_t)fi, sums[fi]);
        if (env_on("EFS_GC_DBG"))
            fprintf(stderr, "raft-host: gc del ino=%llu ci=%u frag=%u node=%u rc=%d\n",
                    (unsigned long long)ino, ci, fi, nodes[fi], rc);
        if (rc != EFS_OK)
            continue; /* real failure: retry next pass */
        acks[*nack].ino = ino;
        acks[*nack].gen = gen;
        acks[*nack].ci = ci;
        acks[*nack].lane = lane;
        acks[*nack].frag = (uint8_t)fi;
        (*nack)++;
    }
}

/* REAP pass over one group's anchor shard: sweep every active lane of
 * each dead inode, then finish the reap on the inode's group. A failed
 * proposal skips the marker — the next pass re-drives it. */
static void host_gc_reap_pass(struct efs_raft_host *h, uint8_t group,
                              uint32_t anchor)
{
    struct reap_scan_ctx c;
    uint8_t prefix[3];
    uint32_t plen = 0;
    int i, lane;

    int prc, src;

    (void)group;
    prc = efs_kv_key_reap_prefix(anchor, prefix, &plen);
    memset(&c, 0, sizeof(c));
    src = (prc == EFS_OK)
          ? efs_kv_scan_prefix(h->kv, prefix, plen, reap_scan_cb, &c)
          : -999;
    if (env_on("EFS_GC_DBG"))
        fprintf(stderr, "raft-host: gc reap pass group=%u anchor=%u prc=%d src=%d markers=%d kv=%p\n",
                group, anchor, prc, src, c.n, (void *)h->kv);
    /* src > 0 is the scan callback's "batch full, stop" signal (merge_scan
     * propagates it), NOT an error — process the partial batch and pick up
     * the rest next pass. Only a negative rc is a real KV failure. */
    if (prc != EFS_OK || src < 0)
        return;
    for (i = 0; i < c.n && h->gc_running && h->running; i++) {
        uint8_t cmd[18];
        int lanes_ok = 1;
        int lrc = 0, drc = 0;

        for (lane = 0; lane < EFS_META_LANES; lane++) {
            uint32_t lshard;

            if (!(c.lanes[i] & (1ull << lane)))
                continue;
            lshard = efs_kv_lane_shard(c.ino[i], (uint8_t)lane);
            cmd[0] = EFS_MD_CMD_LANE_SWEEP;
            wr64be(cmd + 1, c.ino[i]);
            wr64be(cmd + 9, c.gen[i]);
            cmd[17] = (uint8_t)lane;
            lrc = host_gc_propose(h, efs_raft_shard_group(lshard), cmd, 18);
            if (lrc != EFS_OK) {
                lanes_ok = 0;
                break; /* retry the whole marker next pass */
            }
        }
        if (!lanes_ok) {
            if (env_on("EFS_GC_DBG"))
                fprintf(stderr, "raft-host: gc reap ino=%llu lane_sweep rc=%d (retry)\n",
                        (unsigned long long)c.ino[i], lrc);
            continue;
        }
        cmd[0] = EFS_MD_CMD_REAP_DONE;
        wr64be(cmd + 1, c.ino[i]);
        wr64be(cmd + 9, c.gen[i]);
        drc = host_gc_propose(h,
                              efs_raft_shard_group(efs_kv_inode_shard(c.ino[i])),
                              cmd, 17);
        if (env_on("EFS_GC_DBG"))
            fprintf(stderr, "raft-host: gc reap ino=%llu lanes=%llx sweep ok, reap_done rc=%d\n",
                    (unsigned long long)c.ino[i],
                    (unsigned long long)c.lanes[i], drc);
    }
}

/* GC pass over one group's anchor shard: delete fragments for each dead
 * chunk generation, then batch the acks into one GC_ACK entry. */
static void host_gc_frag_pass(struct efs_raft_host *h, uint8_t group,
                              uint32_t anchor)
{
    struct gc_scan_ctx c;
    struct efs_gc_ack_item acks[GC_ACK_MAX];
    struct efs_export *ex;
    uint8_t prefix[3];
    uint32_t plen = 0;
    int nack = 0;
    int i;

    int prc, src;

    prc = efs_kv_key_gc_prefix(anchor, prefix, &plen);
    memset(&c, 0, sizeof(c));
    src = (prc == EFS_OK)
          ? efs_kv_scan_prefix(h->kv, prefix, plen, gc_scan_cb, &c)
          : -999;
    ex = host_gc_export(h);
    if (env_on("EFS_GC_DBG"))
        fprintf(stderr, "raft-host: gc frag pass group=%u anchor=%u prc=%d src=%d records=%d ex=%p\n",
                group, anchor, prc, src, c.n, (void *)ex);
    /* src > 0 is the scan callback's "batch full" stop, not an error. */
    if (prc != EFS_OK || src < 0 || c.n == 0) {
        if (ex)
            server_export_put(h->s, ex);
        return;
    }
    if (!ex)
        return; /* no export yet (pre-mkfs): nothing to delete under */
    for (i = 0; i < c.n && h->gc_running && h->running && nack < GC_ACK_MAX;
         i++)
        host_gc_record(h, ex, c.keys[i], c.vals[i], acks, &nack);
    server_export_put(h->s, ex);
    if (nack > 0) {
        uint8_t cmd[3 + GC_ACK_MAX * 22];
        int j, off = 3;

        cmd[0] = EFS_MD_CMD_GC_ACK;
        cmd[1] = (uint8_t)(nack >> 8);
        cmd[2] = (uint8_t)nack;
        for (j = 0; j < nack; j++) {
            wr64be(cmd + off, acks[j].ino);
            wr64be(cmd + off + 8, acks[j].gen);
            cmd[off + 16] = acks[j].lane;
            wr32be(cmd + off + 17, acks[j].ci);
            cmd[off + 21] = acks[j].frag;
            off += 22;
        }
        (void)host_gc_propose(h, group, cmd, (uint32_t)off);
    }
}

static void host_dir_spread_pass(struct efs_raft_host *h);

static void *host_gc_thread(void *arg)
{
    struct efs_raft_host *h = arg;
    int g;

    while (h->gc_running) {
        for (g = 0; g < HOST_NGROUPS && h->gc_running; g++) {
            uint32_t anchor;
            int lead = 0;

            if (!h->g[g].hosted)
                continue;
            pthread_mutex_lock(&h->mu);
            if (h->g[g].r && efs_raft_role(h->g[g].r) == EFS_RAFT_LEADER)
                lead = 1;
            pthread_mutex_unlock(&h->mu);
            if (env_on("EFS_GC_DBG"))
                fprintf(stderr, "raft-host: gc loop g=%d group=%u r=%p role=%d lead=%d\n",
                        g, h->g[g].group, (void *)h->g[g].r,
                        h->g[g].r ? efs_raft_role(h->g[g].r) : -1, lead);
            if (!lead)
                continue;
            /* Group 0 owns the odd shards (anchor 1), group 2 the even
             * ones (anchor 2) — efs_kv_anchor_shard's parity rule. */
            anchor = (h->g[g].group == 0) ? 1u : 2u;
            host_gc_reap_pass(h, h->g[g].group, anchor);
            host_gc_frag_pass(h, h->g[g].group, anchor);
        }
        host_dir_spread_pass(h);
        /* ~1s between passes, in 20 ms slices so shutdown is prompt. */
        for (g = 0; g < GC_LOOP_MS / 20 && h->gc_running; g++)
            usleep(20 * 1000);
    }
    return NULL;
}

int server_raft_host_start(struct efsd_server *s)
{
    struct efs_raft_host *h;
    char dir[EFS_MAX_PATH];
    const char *ns;
    int n = EFS_MAX_NODES;
    int rc;

    /* Step 11: the Raft+KV engine is the ONLY metadata path — the host
     * starts unconditionally (EFS_MD_RAFT is gone). */
    if (!s || g_host)
        return EFS_ERR_INVAL;
    ns = getenv("EFS_MD_RAFT_N");
    if (ns && ns[0]) {
        n = atoi(ns);
        if (n < 3 || n > EFS_MAX_NODES) {
            fprintf(stderr, "raft-host: EFS_MD_RAFT_N must be 3..%d\n",
                    EFS_MAX_NODES);
            return EFS_ERR_INVAL;
        }
    }
    if (s->id < 1 || (int)s->id > n) {
        fprintf(stderr, "raft-host: node-id %u out of 1..%d\n", s->id, n);
        return EFS_ERR_INVAL;
    }

    h = calloc(1, sizeof(*h));
    if (!h)
        return EFS_ERR_NOMEM;
    h->s = s;
    h->raft_id = (int)s->id - 1;
    h->n = n;
    h->boot_id = make_boot_id();
    h->salt = make_salt(h->boot_id);
    pthread_mutex_init(&h->mu, NULL);
    pthread_mutex_init(&h->read_mu, NULL);
    pthread_mutex_init(&h->wait_mu, NULL);
    pthread_mutex_init(&h->inbox_mu, NULL);
    pthread_mutex_init(&h->outbox_mu, NULL);
    pthread_cond_init(&h->applied_cv, NULL);
    pthread_cond_init(&h->outbox_cv, NULL);
    {
        int i;
        for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
            h->tx[i].h = h;
            h->tx[i].peer = i;
        }
    }
    h->pump_efd = eventfd(0, EFD_NONBLOCK);

    snprintf(dir, sizeof(dir), "%s/mdraft", s->storage_path);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "raft-host: mkdir %s: %s\n", dir, strerror(errno));
        free(h);
        return EFS_ERR_IO;
    }
    rc = host_boot_bump(dir, &h->boot_id);
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: boot persist %s failed\n", dir);
        free(h);
        return rc;
    }
    {
        char kvdir[EFS_MAX_PATH], logdir[EFS_MAX_PATH];
        struct efs_kv_lsm_cfg kcfg;
        snprintf(kvdir, sizeof(kvdir), "%s/kv", dir);
        snprintf(logdir, sizeof(logdir), "%s/log", dir);
        memset(&kcfg, 0, sizeof(kcfg));
        kcfg.sync_mode = EFS_KV_LSM_SYNC;
        h->kv = efs_kv_lsm_open(kvdir, &kcfg);
        if (!h->kv) {
            fprintf(stderr, "raft-host: kv_lsm_open %s failed\n", kvdir);
            free(h);
            return EFS_ERR_IO;
        }
        h->disk = efs_raft_disk_open(logdir, EFS_RAFT_DISK_SYNC);
        if (!h->disk) {
            fprintf(stderr, "raft-host: raft_disk_open %s failed\n", logdir);
            efs_kv_lsm_close(h->kv);
            free(h);
            return EFS_ERR_IO;
        }
    }
    rc = attach_group(h, 0, EFS_RAFT_GROUP_SHARD);
    if (rc == EFS_OK)
        rc = attach_group(h, 1, EFS_RAFT_GROUP_SHARD2);
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: attach failed rc=%d\n", rc);
        efs_raft_free(h->g[0].r);
        efs_raft_free(h->g[1].r);
        efs_raft_disk_close(h->disk);
        efs_kv_lsm_close(h->kv);
        free(h);
        return rc;
    }
    h->tx_running = 1;
    h->running = 1;
    g_host = h;
    if (efsd_pthread_create(&h->tid, host_pump, h) != 0) {
        fprintf(stderr, "raft-host: pump thread failed\n");
        g_host = NULL;
        h->running = 0;
        host_stop_senders(h);
        efs_raft_free(h->g[0].r);
        efs_raft_free(h->g[1].r);
        efs_raft_disk_close(h->disk);
        efs_kv_lsm_close(h->kv);
        free(h);
        return EFS_ERR_IO;
    }
    h->started = 1;
    /* GC reaper after the pump: its proposals need the pump to apply. A
     * spawn failure only leaks fragments (logged), never corrupts.
     * EFS_GC_DISABLE is an operational escape hatch (A/B, emergencies):
     * with it the metadata side still writes reap markers/GC records,
     * they just are never driven to fragment deletes. */
    h->gc_running = 1;
    if (env_on("EFS_GC_DISABLE")) {
        fprintf(stderr, "raft-host: GC reaper DISABLED (EFS_GC_DISABLE); "
                "unlinked fragments will leak\n");
        h->gc_running = 0;
    } else if (efsd_pthread_create(&h->gc_tid, host_gc_thread, h) != 0) {
        fprintf(stderr, "raft-host: GC reaper thread failed; "
                "unlinked fragments will leak\n");
        h->gc_running = 0;
    } else {
        h->gc_started = 1;
    }
    fprintf(stderr,
            "raft-host: up raft_id=%d n=%d boot=%llu salt=%llu "
            "g0=%s g2=%s\n",
            h->raft_id, h->n,
            (unsigned long long)h->boot_id,
            (unsigned long long)h->salt,
            h->g[0].hosted ? "hosted" : "off",
            h->g[1].hosted ? "hosted" : "off");
    return 0;
}

/* Stop every sender thread: tx_running=0 makes host_send drop and each
 * sender drop its queue and exit; join whoever was spawned. Callers must
 * have stopped the producers first (pump joined, connection threads gone)
 * or the remaining sends are simply dropped — safe either way. */
static void host_stop_senders(struct efs_raft_host *h)
{
    int i;

    pthread_mutex_lock(&h->outbox_mu);
    h->tx_running = 0;
    pthread_cond_broadcast(&h->outbox_cv);
    pthread_mutex_unlock(&h->outbox_mu);
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (h->tx[i].started)
            pthread_join(h->tx[i].tid, NULL);
    }
}

void server_raft_host_stop(void)
{
    struct efs_raft_host *h = g_host;
    struct host_lock_wait *w;
    int i;

    if (!h)
        return;
    /* Stop the GC reaper first: an in-flight host_propose_wait needs the
     * pump alive to apply, and its peer RPCs need the conn pool. */
    h->gc_running = 0;
    if (h->gc_started)
        pthread_join(h->gc_tid, NULL);
    h->running = 0;
    /* Wake the pump so shutdown does not wait out a 5 ms tick. */
    host_pump_kick(h);
    pthread_mutex_lock(&h->wait_mu);
    for (w = h->wait_head; w; w = w->next)
        pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&h->wait_mu);
    if (h->started)
        pthread_join(h->tid, NULL);
    /* After the pump join (no new tick sends) and before the raft cores are
     * freed. A sender blocked on a dead peer exits within HOST_SEND_IO_MS. */
    host_stop_senders(h);
    pthread_mutex_lock(&h->mu);
    for (i = 0; i < HOST_NGROUPS; i++) {
        efs_raft_free(h->g[i].r);
        h->g[i].r = NULL;
    }
    pthread_mutex_unlock(&h->mu);
    pthread_mutex_lock(&h->inbox_mu);
    for (i = 0; i < h->inbox_n; i++)
        free(h->inbox[i].buf);
    h->inbox_n = 0;
    pthread_mutex_unlock(&h->inbox_mu);
    efs_raft_disk_close(h->disk);
    efs_kv_lsm_close(h->kv);
    pthread_mutex_destroy(&h->mu);
    pthread_mutex_destroy(&h->read_mu);
    pthread_mutex_destroy(&h->wait_mu);
    pthread_mutex_destroy(&h->inbox_mu);
    pthread_mutex_destroy(&h->outbox_mu);
    pthread_cond_destroy(&h->applied_cv);
    pthread_cond_destroy(&h->outbox_cv);
    if (h->pump_efd >= 0)
        close(h->pump_efd);
    g_host = NULL;
    free(h);
}

int server_raft_host_inbox(const uint8_t *payload, uint32_t plen)
{
    struct efs_raft_host *h = g_host;
    uint8_t *copy;

    if (!h || !h->running)
        return EFS_ERR_INVAL;
    if (!payload || plen < EFS_WIRE_RAFT_HDR_LEN)
        return EFS_ERR_PROTO;
    copy = malloc(plen);
    if (!copy)
        return EFS_ERR_NOMEM;
    memcpy(copy, payload, plen);
    pthread_mutex_lock(&h->inbox_mu);
    if (h->inbox_n >= HOST_INBOX_MAX) {
        pthread_mutex_unlock(&h->inbox_mu);
        free(copy);
        return EFS_ERR_BUSY;
    }
    h->inbox[h->inbox_n].buf = copy;
    h->inbox[h->inbox_n].len = plen;
    h->inbox_n++;
    pthread_mutex_unlock(&h->inbox_mu);
    /* Wake the pump now (not at the next 5 ms tick). Lock-free on purpose:
     * this handler must never queue on h->mu — h->mu serializes all raft
     * core work, and a handler that blocked on it would delay the
     * RAFT_REPLY a peer's sender thread is waiting on. */
    host_pump_kick(h);
    return EFS_OK;
}

void server_raft_host_mkfs(struct efs_msg_raft_mkfs_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_raft *r;
    uint8_t cmd[17];
    uint64_t idx = 0;
    int rc;

    memset(out, 0, sizeof(*out));
    out->leader_hint = -1;
    if (!h || !h->running) {
        out->rc = EFS_ERR_INVAL;
        return;
    }
    out->salt = h->salt;
    cmd[0] = EFS_MD_CMD_MKFS;
    wr64be(cmd + 1, now_ns());
    wr64be(cmd + 9, h->salt);
    pthread_mutex_lock(&h->mu);
    r = group_raft(h, EFS_RAFT_GROUP_SHARD);
    if (!r) {
        pthread_mutex_unlock(&h->mu);
        out->rc = EFS_ERR_NOT_PRIMARY;
        return;
    }
    out->leader_hint = efs_raft_leader(r);
    rc = efs_raft_propose(r, cmd, 17, &idx);
    pthread_mutex_unlock(&h->mu);
    host_pump_kick(h);
    out->rc = rc;
    out->index = idx;
}

static int host_session_get(struct efs_raft_host *h, const uint8_t *cmd,
                            uint32_t clen, uint8_t group, int *hint,
                            uint64_t *salt_out)
{
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t shard = 0;
    struct efs_session_rec rec;
    int rc;

    memcpy(uuid, cmd + 2, EFS_OPID_UUID_LEN);
    if (clen >= HOST_SESS_REG_LEN - 4)
        shard = rd32be(cmd + 18);
    rc = host_read_index(h, group, hint);
    if (rc != EFS_OK)
        return rc;
    rc = efs_session_get(h->kv, uuid, &rec);
    if (rc != EFS_OK)
        return rc;
    /* shard >= 4096: ReadIndex of one 64-bit word of touched[] (35c fence). */
    if (shard >= EFS_SESSION_BITS) {
        uint32_t word = shard - EFS_SESSION_BITS;
        int i;

        if (word >= (EFS_SESSION_BITMAP / 8u))
            return EFS_ERR_INVAL;
        *salt_out = 0;
        for (i = 0; i < 8; i++)
            *salt_out |= (uint64_t)rec.touched[word * 8u + (uint32_t)i]
                         << (8u * (uint32_t)i);
        return EFS_OK;
    }
    *salt_out = (uint64_t)rec.epoch | ((uint64_t)rec.state << 32);
    if (efs_session_bit_get(rec.touched, shard))
        *salt_out |= 1ull << 40;
    return EFS_OK;
}

static int host_dir_migrate(struct efs_raft_host *h, efs_ino_t dir,
                            const uint8_t *cmd, uint32_t clen, int *hint);

/* Payload: group byte, then command bytes (empty command = ReadIndex).
 * A hosted replica proposes (or ReadIndexes) even when it is not the
 * leader; an unhosted node forwards a non-empty command to a voter.
 * Holds read_mu so a submit cannot interleave with a local inode
 * handler on this node. Session GET (sub=0) is a ReadIndex, not a
 * log command: salt carries epoch + state + touched-bit (10.5c-35a). */
void server_raft_host_submit(const uint8_t *payload, uint32_t plen,
                             struct efs_msg_raft_mkfs_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_raft *r;
    uint8_t group;
    const uint8_t *cmd;
    uint32_t clen;
    uint64_t idx = 0;
    uint64_t salt = 0;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->leader_hint = -1;
    if (!h || !h->running || !payload || plen < 1) {
        out->rc = EFS_ERR_INVAL;
        return;
    }
    group = payload[0];
    cmd = payload + 1;
    clen = plen - 1;
    if (clen >= HOST_SESS_HDR_LEN && cmd[0] == EFS_MD_CMD_SESSION &&
        cmd[1] == HOST_SESS_GET) {
        if (!host_hosts(h, group)) {
            if (clen == 0) {
                out->rc = EFS_ERR_NOT_PRIMARY;
                return;
            }
            rc = host_remote_cmd(h, group, cmd, clen, out, -1);
            if (rc != EFS_OK)
                out->rc = rc;
            return;
        }
        pthread_mutex_lock(&h->read_mu);
        rc = host_session_get(h, cmd, clen, group, &hint, &salt);
        pthread_mutex_lock(&h->mu);
        r = group_raft(h, group);
        if (r)
            idx = efs_raft_applied(r);
        pthread_mutex_unlock(&h->mu);
        pthread_mutex_unlock(&h->read_mu);
        out->rc = rc;
        out->index = idx;
        out->salt = salt;
        if (hint >= 0)
            out->leader_hint = hint;
        return;
    }
    if (!host_hosts(h, group)) {
        /* Unhosted: bounce to a voter. ReadIndex still requires a local
         * replica (inode handlers bounce the inode RPC instead). */
        if (clen == 0) {
            out->rc = EFS_ERR_NOT_PRIMARY;
            return;
        }
        rc = host_remote_cmd(h, group, cmd, clen, out, -1);
        if (rc != EFS_OK)
            out->rc = rc;
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    if (clen == 0) {
        rc = host_read_index(h, group, &hint);
        pthread_mutex_lock(&h->mu);
        r = group_raft(h, group);
        if (r)
            idx = efs_raft_applied(r);
        pthread_mutex_unlock(&h->mu);
    } else if (clen >= HOST_DIR_LEN && cmd[0] == EFS_MD_CMD_DIR &&
               cmd[1] == EFS_MD_DIR_MIGRATE) {
        rc = host_dir_migrate(h, rd64be(cmd + 2), cmd, clen, &hint);
        pthread_mutex_lock(&h->mu);
        r = group_raft(h, group);
        if (r)
            idx = efs_raft_applied(r);
        pthread_mutex_unlock(&h->mu);
    } else {
        rc = host_propose_wait(h, group, cmd, clen, &hint);
        pthread_mutex_lock(&h->mu);
        r = group_raft(h, group);
        if (r)
            idx = efs_raft_applied(r);
        pthread_mutex_unlock(&h->mu);
    }
    pthread_mutex_unlock(&h->read_mu);
    out->rc = rc;
    out->index = idx;
    if (hint >= 0)
        out->leader_hint = hint;
}

void server_raft_host_status(struct efs_msg_raft_status_reply *out)
{
    struct efs_raft_host *h = g_host;
    int i;
    uint64_t salt = 0;
    int src;

    memset(out, 0, sizeof(*out));
    if (!h || !h->running) {
        out->rc = EFS_ERR_INVAL;
        return;
    }
    out->rc = EFS_OK;
    out->node_id = h->s ? h->s->id : 0;
    pthread_mutex_lock(&h->mu);
    out->ngroups = HOST_NGROUPS;
    for (i = 0; i < HOST_NGROUPS; i++) {
        struct efs_raft_group_status *gs = &out->groups[i];
        gs->group = h->g[i].group;
        gs->hosted = h->g[i].hosted;
        gs->voters = h->g[i].voters;
        gs->leader = -1;
        if (h->g[i].hosted && h->g[i].r) {
            gs->role = (uint8_t)efs_raft_role(h->g[i].r);
            gs->leader = efs_raft_leader(h->g[i].r);
            gs->term = efs_raft_term(h->g[i].r);
            gs->commit_index = efs_raft_commit(h->g[i].r);
            gs->applied_index = efs_raft_applied(h->g[i].r);
        }
    }
    pthread_mutex_unlock(&h->mu);
    src = efs_meta_apply_export_salt(h->kv, &salt);
    if (src == EFS_OK) {
        struct efs_meta_row row;
        if (efs_meta_apply_get_inode(h->kv, EFS_ROOT_INO, &row) == EFS_OK) {
            out->kv_has_root = 1;
            out->export_salt = salt;
        }
    }
}

int server_raft_host_active(void)
{
    struct efs_raft_host *h = g_host;
    return h && h->running;
}

void server_raft_host_getattr(efs_ino_t ino, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_stat st;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    {
        uint8_t ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
        if (!host_hosts(h, ig)) {
            host_fwd_getattr(h, ino, out, &ig, 1);
            return;
        }
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        uint8_t need[2];

        pthread_mutex_unlock(&h->read_mu);
        host_need_both(need);
        host_fwd_getattr(h, ino, out, need, 2);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

/* Open lease (I19): one Raft entry on the inode shard. flags=1 open,
 * flags=0 close. owner is the process/ofd id. Optional sess_uuid is
 * the 35b session identity (accept before propose); NULL keeps the
 * stand-in. Directories INVAL. Last close reclaims a nlink=0 inode. */
void server_raft_host_hold(efs_ino_t ino, uint32_t flags, uint64_t owner,
                           const uint8_t *sess_uuid, uint32_t sess_epoch,
                           struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_stat st;
    uint8_t cmd[HOST_SESS_LEASE_LEN];
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t clen = 0, epoch = 1;
    uint8_t ig;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (ino == 0 || (flags != 0 && flags != 1)) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, ig)) {
        host_fwd_hold(h, ino, flags, owner, sess_uuid, sess_epoch, out, &ig, 1);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, ig, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        pthread_mutex_unlock(&h->read_mu);
        host_fwd_hold(h, ino, flags, owner, sess_uuid, sess_epoch, out, &ig, 1);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = host_sess_gate(h, ino, sess_uuid, sess_epoch);
    if (rc == EFS_OK) {
        host_sess_bytes(owner, sess_uuid, sess_epoch, uuid, &epoch);
        rc = pack_lease_cmd(cmd, &clen, flags ? 1 : 0, ino, row.generation,
                            uuid, epoch);
    }
    if (rc == EFS_OK)
        rc = host_propose_wait(h, ig, cmd, clen, &hint);
    if (rc == EFS_OK) {
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
        if (flags == 0 && rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK && flags == 1)
        stat_to_inode(&st, &out->inode);
}

/* Blocking lock waits (10.5c-34, §7.6). EFS_FLOCK_WAIT on a conflicting
 * GRANT queues the request at the leader and holds the RPC: the reply
 * IS the grant, and the grant itself is still a Raft record. The queue
 * is leader memory (not Raft state), FIFO per inode; a queued waiter
 * blocks later conflicting requests, so an exclusive waiter cannot be
 * starved by shared grants. Leader loss replies NOT_PRIMARY and the
 * client re-issues (the queue rebuilds there). read_mu is never held
 * while sleeping; wait_mu nests inside read_mu, never the reverse. */

#define HOST_LOCK_WAIT_MS 50

static int host_is_leader(struct efs_raft_host *h, uint8_t group)
{
    struct efs_raft *r;
    int ret = 0;

    pthread_mutex_lock(&h->mu);
    r = group_raft(h, group);
    if (r && efs_raft_role(r) == EFS_RAFT_LEADER)
        ret = 1;
    pthread_mutex_unlock(&h->mu);
    return ret;
}

/* Current leader hint for a group (-1 unknown). */
static int host_leader_hint(struct efs_raft_host *h, uint8_t group)
{
    struct efs_raft *r;
    int lid = -1;

    pthread_mutex_lock(&h->mu);
    r = group_raft(h, group);
    if (r)
        lid = efs_raft_leader(r);
    pthread_mutex_unlock(&h->mu);
    return lid;
}

/* A queued waiter blocks a later conflicting request (same inode and
 * domain, overlapping range, different owner, one side EX). Caller
 * holds wait_mu. */
static int lock_wait_queued_locked(struct efs_raft_host *h,
                                   const struct efs_lock_req *r)
{
    struct host_lock_wait *w;

    for (w = h->wait_head; w; w = w->next) {
        if (w->ino != r->ino || w->domain != r->domain)
            continue;
        if (w->owner == r->owner.id)
            continue;
        if (w->start >= r->end || r->start >= w->end)
            continue;
        if (w->ltype == EFS_LOCK_EX || r->type == EFS_LOCK_EX)
            return 1;
    }
    return 0;
}

/* First queued waiter for its inode (strict FIFO). Caller holds wait_mu. */
static int lock_wait_is_head_locked(struct efs_raft_host *h,
                                    const struct host_lock_wait *w)
{
    struct host_lock_wait *q;

    for (q = h->wait_head; q; q = q->next)
        if (q->ino == w->ino)
            return q == w;
    return 0;
}

/* Wake every waiter on an inode (a release or grant may have changed
 * what is grantable). Caller holds wait_mu. */
static void lock_wait_signal_locked(struct efs_raft_host *h, efs_ino_t ino)
{
    struct host_lock_wait *w;

    for (w = h->wait_head; w; w = w->next)
        if (w->ino == ino)
            pthread_cond_broadcast(&w->cv);
}

static void lock_wait_unlink_locked(struct efs_raft_host *h,
                                    struct host_lock_wait *w)
{
    struct host_lock_wait **pp = &h->wait_head;
    struct host_lock_wait *t;

    while (*pp && *pp != w)
        pp = &(*pp)->next;
    if (*pp)
        *pp = w->next;
    if (h->wait_tail == w) {
        h->wait_tail = NULL;
        for (t = h->wait_head; t; t = t->next)
            if (!t->next)
                h->wait_tail = t;
    }
}

/* Queue behind the current holders until the request is grantable, then
 * propose the GRANT and return its result. The 50 ms tick is only a
 * liveness recheck (leader loss / shutdown); grants are woken by signal. */
static int host_lock_wait(struct efs_raft_host *h, uint8_t ig,
                          const struct efs_lock_req *req, int *hint)
{
    struct host_lock_wait w;
    uint8_t cmd[HOST_LOCK_LEN];
    uint32_t clen = 0;
    int rc, blk, head;

    memset(&w, 0, sizeof(w));
    w.ino = req->ino;
    w.owner = req->owner.id;
    memcpy(w.uuid, req->owner.uuid, EFS_OPID_UUID_LEN);
    w.epoch = req->owner.epoch;
    w.domain = req->domain;
    w.ltype = req->type;
    w.start = req->start;
    w.end = req->end;
    pthread_cond_init(&w.cv, NULL);

    pthread_mutex_lock(&h->wait_mu);
    if (h->wait_tail)
        h->wait_tail->next = &w;
    else
        h->wait_head = &w;
    h->wait_tail = &w;
    pthread_mutex_unlock(&h->wait_mu);

    for (;;) {
        if (!h->running || !host_is_leader(h, ig))
            w.abort = 1;
        pthread_mutex_lock(&h->wait_mu);
        if (w.fenced) {
            lock_wait_unlink_locked(h, &w);
            lock_wait_signal_locked(h, w.ino);
            pthread_mutex_unlock(&h->wait_mu);
            pthread_cond_destroy(&w.cv);
            return EFS_ERR_STALE;
        }
        if (w.abort) {
            lock_wait_unlink_locked(h, &w);
            lock_wait_signal_locked(h, w.ino);
            pthread_mutex_unlock(&h->wait_mu);
            pthread_cond_destroy(&w.cv);
            if (hint)
                *hint = host_leader_hint(h, ig);
            return EFS_ERR_NOT_PRIMARY;
        }
        head = lock_wait_is_head_locked(h, &w);
        pthread_mutex_unlock(&h->wait_mu);
        if (head) {
            pthread_mutex_lock(&h->read_mu);
            rc = host_read_index(h, ig, hint);
            if (rc == EFS_OK) {
                blk = efs_lock_blocked(h->kv, req, NULL);
                if (blk < 0)
                    rc = blk;
                else if (blk)
                    rc = EFS_ERR_AGAIN;
            }
            if (rc == EFS_OK) {
                rc = pack_lock_cmd(cmd, &clen, EFS_MD_LOCK_GRANT, req);
                if (rc == EFS_OK)
                    rc = host_propose_wait(h, ig, cmd, clen, hint);
                if (rc == EFS_OK) {
                    blk = efs_lock_blocked(h->kv, req, NULL);
                    if (blk < 0)
                        rc = blk;
                    else if (blk)
                        rc = EFS_ERR_AGAIN;
                }
            }
            pthread_mutex_unlock(&h->read_mu);
            if (rc != EFS_ERR_AGAIN) {
                int fenced;

                pthread_mutex_lock(&h->wait_mu);
                fenced = w.fenced;
                lock_wait_unlink_locked(h, &w);
                lock_wait_signal_locked(h, w.ino);
                pthread_mutex_unlock(&h->wait_mu);
                pthread_cond_destroy(&w.cv);
                return fenced ? EFS_ERR_STALE : rc;
            }
        }
        pthread_mutex_lock(&h->wait_mu);
        if (!w.abort && !w.fenced) {
            struct timespec ts;

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += HOST_LOCK_WAIT_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&w.cv, &h->wait_mu, &ts);
        }
        pthread_mutex_unlock(&h->wait_mu);
    }
}

/* Revocation-barrier hook (used by session fencing, 10.5c-35): a fenced
 * session's waiters are dequeued and never granted (§7.6). */
static void host_lock_wait_drop_session(struct efs_raft_host *h,
                                        const uint8_t uuid[EFS_OPID_UUID_LEN],
                                        uint32_t epoch)
{
    struct host_lock_wait *w;

    if (!h || !uuid)
        return;
    pthread_mutex_lock(&h->wait_mu);
    for (w = h->wait_head; w; w = w->next) {
        if (w->epoch != epoch)
            continue;
        if (memcmp(w->uuid, uuid, EFS_OPID_UUID_LEN) != 0)
            continue;
        w->fenced = 1;
        w->abort = 1;
        pthread_cond_broadcast(&w->cv);
    }
    pthread_mutex_unlock(&h->wait_mu);
}

void server_raft_host_lock_wait_drop_owner(efs_ino_t ino, uint64_t owner)
{
    struct efs_raft_host *h = g_host;
    struct host_lock_wait *w;

    if (!h)
        return;
    pthread_mutex_lock(&h->wait_mu);
    for (w = h->wait_head; w; w = w->next)
        if (w->ino == ino && w->owner == owner) {
            w->abort = 1;
            pthread_cond_broadcast(&w->cv);
        }
    pthread_mutex_unlock(&h->wait_mu);
}

/* flock/fcntl on the inode shard (§7.6). EFS_FLOCK_FCNTL
 * selects the record-lock domain (byte ranges allowed); otherwise
 * FLOCK (whole-file only). owner is the process/ofd id; optional
 * sess_uuid is the 35b session (accept before GRANT/RELEASE).
 * Conflict → BUSY, or a queued blocking wait with EFS_FLOCK_WAIT.
 * EFS_FLOCK_GETLK is a ReadIndex (no Raft entry, no accept). */
void server_raft_host_flock(efs_ino_t ino, uint32_t op, uint64_t owner,
                            uint64_t start, uint64_t end,
                            const uint8_t *sess_uuid, uint32_t sess_epoch,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_lock_req req;
    struct efs_lock_req hit;
    uint8_t cmd[HOST_LOCK_LEN];
    uint32_t clen = 0;
    uint8_t ig;
    uint8_t kind;
    uint8_t ltype;
    uint8_t domain;
    int hint = -1;
    int rc;
    int blk;
    int is_getlk = (op & EFS_FLOCK_GETLK) ? 1 : 0;

    memset(out, 0, sizeof(*out));
    memset(&hit, 0, sizeof(hit));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (start >= end) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (is_getlk) {
        if (op & EFS_FLOCK_UN) {
            out->status = EFS_INODE_RPC_INVAL;
            return;
        }
        kind = 0;
        if (op & EFS_FLOCK_EX)
            ltype = EFS_LOCK_EX;
        else if (op & EFS_FLOCK_SH)
            ltype = EFS_LOCK_SH;
        else {
            out->status = EFS_INODE_RPC_INVAL;
            return;
        }
    } else if (op & EFS_FLOCK_UN) {
        kind = EFS_MD_LOCK_RELEASE;
        ltype = EFS_LOCK_EX;
    } else if (op & EFS_FLOCK_EX) {
        kind = EFS_MD_LOCK_GRANT;
        ltype = EFS_LOCK_EX;
    } else if (op & EFS_FLOCK_SH) {
        kind = EFS_MD_LOCK_GRANT;
        ltype = EFS_LOCK_SH;
    } else {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if ((op & EFS_FLOCK_WAIT) && kind != EFS_MD_LOCK_GRANT) {
        /* WAIT only modifies a grant (never UN, never GETLK). */
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    domain = (op & EFS_FLOCK_FCNTL) ? EFS_LOCK_FCNTL : EFS_LOCK_FLOCK;
    if (domain == EFS_LOCK_FLOCK &&
        (start != 0 || end != ~(uint64_t)0)) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, ig)) {
        host_fwd_flock(h, ino, op, owner, start, end, sess_uuid, sess_epoch,
                       out, &ig, 1);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, ig, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        pthread_mutex_unlock(&h->read_mu);
        host_fwd_flock(h, ino, op, owner, start, end, sess_uuid, sess_epoch,
                       out, &ig, 1);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && !is_getlk)
        rc = host_sess_gate(h, ino, sess_uuid, sess_epoch);
    if (rc == EFS_OK) {
        fill_flock_req(&req, ino, row.generation, ltype, owner, domain,
                       start, end, sess_uuid, sess_epoch);
        if (is_getlk) {
            memset(&hit, 0, sizeof(hit));
            rc = efs_lock_getlk(h->kv, &req, &hit);
        } else if (kind == EFS_MD_LOCK_GRANT) {
            blk = efs_lock_blocked(h->kv, &req, NULL);
            if (blk < 0)
                rc = blk;
            else if (blk)
                rc = EFS_ERR_AGAIN;
            else {
                /* A queued waiter blocks a later conflicting grant
                 * (no barging), whether or not the new request waits. */
                pthread_mutex_lock(&h->wait_mu);
                blk = lock_wait_queued_locked(h, &req);
                pthread_mutex_unlock(&h->wait_mu);
                if (blk)
                    rc = EFS_ERR_AGAIN;
            }
        }
    }
    if (!is_getlk) {
        if (rc == EFS_OK)
            rc = pack_lock_cmd(cmd, &clen, kind, &req);
        if (rc == EFS_OK)
            rc = host_propose_wait(h, ig, cmd, clen, &hint);
        if (rc == EFS_OK && kind == EFS_MD_LOCK_GRANT) {
            blk = efs_lock_blocked(h->kv, &req, NULL);
            if (blk < 0)
                rc = blk;
            else if (blk)
                rc = EFS_ERR_AGAIN;
        }
        if (rc == EFS_OK && kind == EFS_MD_LOCK_RELEASE) {
            /* A release may have freed a waiter's range. */
            pthread_mutex_lock(&h->wait_mu);
            lock_wait_signal_locked(h, ino);
            pthread_mutex_unlock(&h->wait_mu);
        }
    }
    pthread_mutex_unlock(&h->read_mu);
    if (rc == EFS_ERR_AGAIN && kind == EFS_MD_LOCK_GRANT &&
        (op & EFS_FLOCK_WAIT))
        rc = host_lock_wait(h, ig, &req, &hint);
    set_inode_rc(out, rc, hint);
    if (is_getlk && rc == EFS_OK) {
        out->inode.nlink = hit.type;
        out->inode.size = hit.start;
        out->inode.ctime = hit.end;
        out->inode.ino = (efs_ino_t)hit.owner.id;
    }
}

void server_raft_host_lookup(efs_ino_t parent, const char *name,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    uint32_t psh;
    uint8_t pg;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    psh = efs_kv_inode_shard(parent);
    pg = efs_raft_shard_group(psh);
    if (!host_hosts(h, pg)) {
        uint8_t need[2];
        need[0] = pg;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_lookup(h, parent, name, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, pg, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL) {
        uint32_t hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        uint8_t hg = efs_raft_shard_group(hsh);
        int hh = -1;
        if (hg != pg) {
            if (!host_hosts(h, hg)) {
                uint8_t need[2];
                need[0] = pg;
                need[1] = hg;
                pthread_mutex_unlock(&h->read_mu);
                host_fwd_lookup(h, parent, name, out, need, 2);
                return;
            }
            rc = host_read_index(h, hg, &hh);
            if (rc != EFS_OK)
                hint = hh;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK) {
        uint8_t cg = efs_raft_shard_group(efs_kv_inode_shard(dent.ino));
        if (!host_hosts(h, cg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            if (cg != pg)
                    need[nn++] = cg;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_lookup(h, parent, name, out, need, nn);
            return;
        }
        rc = host_read_inode_lanes(h, dent.ino, &hint);
        if (rc == EFS_ERR_NOT_PRIMARY) {
            uint8_t need[2];

            pthread_mutex_unlock(&h->read_mu);
            host_need_both(need);
            host_fwd_lookup(h, parent, name, out, need, 2);
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, dent.ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* File CREATE: one Raft entry on the dentry shard when that shard's
 * group already has everything the apply writes (LOCAL parent, or a
 * HASHED/SPLITTING dest whose used_shards bit is set, or first-use on
 * the same Raft group as the parent). First use of a hashed dir lane
 * on another group is a 2-shard txn (parent used_shards + dest
 * dentry/inode). SPLITTING dest writes hashed (spec: writes go hashed;
 * reads hashed-then-local). MKDIR is a 2-shard txn of its own. */
static int host_hashed_create_txn(struct efs_raft_host *h, efs_ino_t parent,
                                  const char *name, uint32_t mode,
                                  const struct efs_meta_attrs *at,
                                  struct efs_meta_row *prow, uint32_t dsh,
                                  int *hint);

void server_raft_host_create(efs_ino_t parent, const char *name, uint32_t mode,
                             uint32_t uid, uint32_t gid,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_meta_attrs at;
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t clen = 0;
    uint64_t idx = 0;
    uint32_t dsh = 0;
    uint8_t pg, dg = 0;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if ((mode & S_IFMT) == 0)
        mode |= S_IFREG;
    if (S_ISDIR(mode)) {
        server_raft_host_mkdir(parent, name, mode, uid, gid, out);
        return;
    }
    at.uid = uid;
    at.gid = gid;
    at.now = now_ns();
    rc = pack_create_cmd(cmd, &clen, parent, mode, name, &at);
    if (rc != EFS_OK) {
        set_inode_rc(out, rc, -1);
        return;
    }
    pg = efs_raft_shard_group(efs_kv_inode_shard(parent));
    if (!host_hosts(h, pg)) {
        uint8_t need[2];
        need[0] = pg;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_create(h, parent, name, mode, uid, gid, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, pg, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        dg = efs_raft_shard_group(dsh);
        if (!host_hosts(h, dg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            if (dg != pg)
                need[nn++] = dg;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_create(h, parent, name, mode, uid, gid, out, need, nn);
            return;
        }
        if (dg != pg) {
            int hh = -1;
            rc = host_read_index(h, dg, &hh);
            if (rc != EFS_OK)
                hint = hh;
        }
    }
    if (rc == EFS_OK) {
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
        if (rc == EFS_OK) {
            pthread_mutex_unlock(&h->read_mu);
            set_inode_rc(out, EFS_ERR_EXIST, hint);
            return;
        }
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
    }
    if (rc == EFS_OK &&
        (prow.layout == EFS_META_LAYOUT_HASHED ||
         prow.layout == EFS_META_LAYOUT_SPLITTING) &&
        (prow.used_shards & (1ull << efs_kv_dir_lane(name))) == 0 &&
        pg != dg)
        rc = host_hashed_create_txn(h, parent, name, mode, &at, &prow, dsh,
                                    &hint);
    else if (rc == EFS_OK) {
        rc = host_propose(h, dg, cmd, clen, &idx, &hint);
        if (rc == EFS_OK)
            rc = host_wait_applied(h, dg, idx, &hint);
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, dent.ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, dent.ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

static int host_parts_add(struct efs_txn_parts *p, uint32_t shard)
{
    uint8_t i;

    if (!p)
        return EFS_ERR_INVAL;
    for (i = 0; i < p->n; i++) {
        if (p->shard[i] == shard)
            return EFS_OK;
    }
    if (p->n >= EFS_TXN_MAX_PART)
        return EFS_ERR_BUSY;
    p->shard[p->n++] = shard;
    return EFS_OK;
}

struct host_pver_guard {
    uint32_t shard;
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen;
    uint64_t ver;
};

/* Ancestry of dst_parent as shared pver GUARDs (not exclusive on the
 * inode row). src in the chain is INVAL. Too many distinct shards is
 * BUSY (EFS_TXN_MAX_PART). read_mu held. */
static int host_pver_guard_chain(struct efs_raft_host *h, efs_ino_t dst_parent,
                                 efs_ino_t src, struct efs_txn_parts *parts,
                                 struct host_pver_guard *g, int *ng, int *hint)
{
    efs_ino_t cur = dst_parent;
    int hops, rc;

    *ng = 0;
    for (hops = 0; hops < 64; hops++) {
        struct efs_meta_row r;
        uint32_t sh;

        if (cur == src)
            return EFS_ERR_INVAL;
        sh = efs_kv_inode_shard(cur);
        rc = host_read_index(h, efs_raft_shard_group(sh), hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_get_inode(h->kv, cur, &r);
        if (rc != EFS_OK)
            return rc;
        if (!S_ISDIR(r.mode))
            return EFS_ERR_INVAL;
        if (*ng >= EFS_TXN_MAX_PART)
            return EFS_ERR_BUSY;
        rc = efs_kv_key_pver(sh, cur, g[*ng].key, &g[*ng].klen);
        if (rc == EFS_OK)
            rc = efs_txn_ver_get(h->kv, g[*ng].key, g[*ng].klen, &g[*ng].ver);
        if (rc == EFS_OK)
            rc = host_parts_add(parts, sh);
        if (rc != EFS_OK)
            return rc;
        g[*ng].shard = sh;
        (*ng)++;
        if (cur == EFS_ROOT_INO || cur == r.parent)
            return EFS_OK;
        cur = r.parent;
    }
    return EFS_ERR_INVAL;
}

static int host_prep(struct efs_raft_host *h, uint32_t shard, int kind,
                     const struct efs_txid *t, const struct efs_txn_parts *p,
                     const uint8_t *key, uint32_t klen, uint64_t expected,
                     int op, const uint8_t *val, uint32_t vlen, int *hint)
{
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t n;

    n = pack_prep(cmd, kind, t, p, key, klen, expected, op, val, vlen);
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    return host_propose_wait(h, efs_raft_shard_group(shard), cmd, n, hint);
}

static int host_drop_parts(struct efs_raft_host *h, const struct efs_txid *t,
                           const struct efs_txn_parts *p, int *hint)
{
    uint8_t cmd[21];
    int rc = EFS_OK, i, one;

    for (i = 0; i < p->n; i++) {
        pack_drop(cmd, t, p->shard[i]);
        one = host_propose_wait(h, efs_raft_shard_group(p->shard[i]), cmd, 21,
                                hint);
        if (one != EFS_OK && rc == EFS_OK)
            rc = one;
    }
    return rc;
}

/* Cross-group leftover migrate: hashed PUT + local DEL + used_shards
 * as a txn so group-2-only replicas see the hashed dentry. Same-group
 * leftovers (or hashed already present) stay a single DIR_MIGRATE apply.
 * read_mu held. Bounces to a dual-host when this replica does not host
 * the dest group. */
static int host_dir_migrate_txn(struct efs_raft_host *h, efs_ino_t dir,
                                const char *name, uint32_t hsh, int *hint)
{
    struct efs_meta_row row;
    struct efs_meta_dentry dent;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_ino[EFS_META_INO_BYTES], v_dseq[8];
    uint8_t loc_buf[EFS_META_DENT_BYTES], sb[8], cmd[22];
    uint32_t kl = 0, kh = 0, ki = 0, ks = 0, locn, sn = 8;
    uint32_t psh, coord;
    uint64_t loc_ver = 0, hash_ver = 0, ino_ver = 0, sver = 0, seq = 0, bit;
    uint8_t lane;
    int rc, i, gr, stamp_ino = 0;

    psh = efs_kv_inode_shard(dir);
    lane = efs_kv_dir_lane(name);
    bit = 1ull << lane;
    rc = efs_meta_apply_get_inode(h->kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_dentry(psh, dir, name, k_loc, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, dir, name, k_hash, &kh);
    if (rc != EFS_OK)
        return rc;
    locn = sizeof(loc_buf);
    rc = efs_kv_get(h->kv, k_loc, kl, loc_buf, &locn);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_unpack_dentry(loc_buf, locn, &dent);
    if (rc == EFS_OK)
        rc = efs_meta_pack_dentry(&dent, v_dent, sizeof(v_dent));
    if (rc != EFS_OK)
        return rc;
    if ((row.used_shards & bit) == 0) {
        row.used_shards |= bit;
        stamp_ino = 1;
        rc = efs_kv_key_inode(psh, dir, k_ino, &ki);
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
        if (rc != EFS_OK)
            return rc;
    }
    rc = efs_kv_key_dseq(hsh, dir, lane, k_dseq, &ks);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(h->kv, k_loc, kl, &loc_ver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_hash, kh, &hash_ver);
    if (rc == EFS_OK && stamp_ino)
        rc = efs_txn_ver_get(h->kv, k_ino, ki, &ino_ver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc != EFS_OK)
        return rc;
    sn = 8;
    gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
    seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
    wr64be(v_dseq, seq + 1);
    memset(&parts, 0, sizeof(parts));
    rc = host_parts_add(&parts, psh);
    if (rc == EFS_OK)
        rc = host_parts_add(&parts, hsh);
    if (rc != EFS_OK)
        return rc;
    fill_txid(h, &t);
    for (i = 0; i < parts.n && rc == EFS_OK; i++) {
        uint32_t sh = parts.shard[i];

        if (sh == psh) {
            rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_loc, kl, loc_ver,
                           EFS_TXN_DEL, NULL, 0, hint);
            if (rc == EFS_OK && stamp_ino)
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                               ino_ver, EFS_TXN_PUT, v_ino, sizeof(v_ino),
                               hint);
        }
        if (rc == EFS_OK && sh == hsh) {
            rc = host_prep(h, hsh, EFS_TXN_EXCL, &t, &parts, k_hash, kh,
                           hash_ver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                           hint);
            if (rc == EFS_OK)
                rc = host_prep(h, hsh, EFS_TXN_EXCL, &t, &parts, k_dseq, ks,
                               sver, EFS_TXN_PUT, v_dseq, 8, hint);
        }
    }
    if (rc != EFS_OK)
        (void)host_drop_parts(h, &t, &parts, hint);
    else {
        coord = efs_txn_coordinator(&t, &parts);
        pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
        rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22, hint);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]), cmd,
                                   22, hint);
        }
    }
    return rc;
}

static int host_dir_migrate(struct efs_raft_host *h, efs_ino_t dir,
                            const uint8_t *cmd, uint32_t clen, int *hint)
{
    char name[EFS_MAX_NAME];
    uint8_t hk[EFS_KV_KEY_MAX], hv[EFS_META_DENT_BYTES], need[2];
    uint32_t hsh = 0, hklen = 0, hvlen, psh;
    uint8_t pg, hg;
    struct efs_msg_raft_mkfs_reply rep;
    int rc, saw = 0, rid;

    psh = efs_kv_inode_shard(dir);
    pg = efs_raft_shard_group(psh);
    rc = host_read_index(h, pg, hint);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_dir_migrate_peek(h->kv, dir, name, sizeof(name), &hsh, &saw);
    if (rc == EFS_ERR_INVAL)
        return host_propose_wait(h, pg, cmd, clen, hint);
    if (rc == EFS_ERR_NOT_FOUND) {
        struct efs_meta_row row;

        if (saw && efs_meta_apply_get_inode(h->kv, dir, &row) == EFS_OK &&
            (row.used_shards & 1ull) == 0)
            return host_propose_wait(h, pg, cmd, clen, hint);
        return EFS_ERR_NOT_FOUND;
    }
    if (rc != EFS_OK)
        return rc;
    hg = efs_raft_shard_group(hsh);
    if (hg != pg && !host_hosts(h, hg)) {
        need[0] = pg;
        need[1] = hg;
        pthread_mutex_unlock(&h->read_mu);
        rid = host_pick_peer(h, need, 2, -1);
        if (rid < 0)
            rc = EFS_ERR_NOT_PRIMARY;
        else
            rc = host_rpc_submit(h, rid, pg, cmd, clen, &rep);
        pthread_mutex_lock(&h->read_mu);
        if (rc != EFS_OK)
            return rc;
        if (hint && rep.leader_hint >= 0)
            *hint = (int)rep.leader_hint;
        return rep.rc;
    }
    if (hg != pg) {
        rc = host_read_index(h, hg, hint);
        if (rc != EFS_OK)
            return rc;
    }
    if (hg == pg)
        return host_propose_wait(h, pg, cmd, clen, hint);
    rc = efs_kv_key_dentry(hsh, dir, name, hk, &hklen);
    if (rc != EFS_OK)
        return rc;
    hvlen = sizeof(hv);
    rc = efs_kv_get(h->kv, hk, hklen, hv, &hvlen);
    if (rc == EFS_OK)
        return host_propose_wait(h, pg, cmd, clen, hint);
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    return host_dir_migrate_txn(h, dir, name, hsh, hint);
}

/* One leftover (or FINISH) per queued SPLITTING dir. Reuses host_dir_migrate
 * so cross-group leftovers stay a txn. No new thread — GC wakes us. */
static void host_dir_spread_pass(struct efs_raft_host *h)
{
    efs_ino_t dir;
    uint8_t cmd[HOST_DIR_LEN];
    uint8_t pg;
    struct efs_raft *r;
    int hint, lead, n, rc;

    for (n = 0; n < HOST_SPREAD_MAX; n++) {
        if (!efs_dir_spread_pop(&dir))
            return;
        pg = efs_raft_shard_group(efs_kv_inode_shard(dir));
        if (!host_hosts(h, pg))
            continue;
        pthread_mutex_lock(&h->mu);
        r = group_raft(h, pg);
        lead = r && efs_raft_role(r) == EFS_RAFT_LEADER;
        pthread_mutex_unlock(&h->mu);
        if (!lead) {
            efs_dir_spread_note(dir);
            continue;
        }
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = EFS_MD_CMD_DIR;
        cmd[1] = EFS_MD_DIR_MIGRATE;
        wr64be(cmd + 2, dir);
        hint = -1;
        pthread_mutex_lock(&h->read_mu);
        rc = host_read_index(h, pg, &hint);
        if (rc == EFS_OK) {
            struct efs_meta_row row;

            rc = efs_meta_apply_get_inode(h->kv, dir, &row);
            if (rc != EFS_OK || row.layout != EFS_META_LAYOUT_SPLITTING) {
                pthread_mutex_unlock(&h->read_mu);
                continue;
            }
            rc = host_dir_migrate(h, dir, cmd, HOST_DIR_LEN, &hint);
        } else {
            pthread_mutex_unlock(&h->read_mu);
            efs_dir_spread_note(dir);
            return;
        }
        if (rc == EFS_ERR_NOT_FOUND) {
            cmd[1] = EFS_MD_DIR_FINISH;
            rc = host_propose_wait(h, pg, cmd, HOST_DIR_LEN, &hint);
            pthread_mutex_unlock(&h->read_mu);
            if (rc != EFS_OK)
                efs_dir_spread_note(dir);
            continue;
        }
        pthread_mutex_unlock(&h->read_mu);
        efs_dir_spread_note(dir);
        if (rc != EFS_OK)
            return;
    }
}

/* LOCAL / HASHED / SPLITTING name drop matching apply dentry_drop_items
 * and sim drop_dentry_prep. SPLITTING writes HASHED=TOMBSTONE(epoch)
 * (I8) and DELs the local leftover unless the keys alias (lane 0). */
struct host_dent_drop {
    uint32_t psh, hsh;
    uint8_t k_loc[EFS_KV_KEY_MAX];
    uint8_t k_hash[EFS_KV_KEY_MAX];
    uint8_t v_tomb[EFS_META_DENT_BYTES];
    uint32_t kl, kh;
    uint64_t loc_ver, hash_ver;
    int del_loc, put_tomb, del_hash;
};

static int host_dent_drop_fill(struct efs_kv *kv, const struct efs_meta_row *prow,
                               efs_ino_t parent, const char *name,
                               struct host_dent_drop *d)
{
    struct efs_meta_dentry tomb;
    int rc;

    memset(d, 0, sizeof(*d));
    d->psh = efs_kv_inode_shard(parent);
    d->hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = efs_kv_key_dentry(d->psh, parent, name, d->k_loc, &d->kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(d->hsh, parent, name, d->k_hash, &d->kh);
    if (rc != EFS_OK)
        return rc;
    if (prow->layout != EFS_META_LAYOUT_HASHED &&
        !(prow->layout == EFS_META_LAYOUT_SPLITTING && d->kl == d->kh &&
          memcmp(d->k_loc, d->k_hash, d->kl) == 0)) {
        d->del_loc = 1;
        rc = efs_txn_ver_get(kv, d->k_loc, d->kl, &d->loc_ver);
        if (rc != EFS_OK)
            return rc;
    }
    if (prow->layout == EFS_META_LAYOUT_SPLITTING) {
        memset(&tomb, 0, sizeof(tomb));
        tomb.generation = prow->layout_epoch;
        tomb.type = EFS_META_DENT_TOMBSTONE;
        rc = efs_meta_pack_dentry(&tomb, d->v_tomb, sizeof(d->v_tomb));
        if (rc != EFS_OK)
            return rc;
        d->put_tomb = 1;
        return efs_txn_ver_get(kv, d->k_hash, d->kh, &d->hash_ver);
    }
    if (prow->layout == EFS_META_LAYOUT_HASHED) {
        d->del_hash = 1;
        return efs_txn_ver_get(kv, d->k_hash, d->kh, &d->hash_ver);
    }
    return EFS_OK;
}

static int host_dent_drop_prep(struct efs_raft_host *h, uint32_t sh,
                               const struct host_dent_drop *d,
                               const struct efs_txid *t,
                               const struct efs_txn_parts *parts, int *hint)
{
    int rc = EFS_OK;

    if (d->put_tomb && sh == d->hsh)
        rc = host_prep(h, d->hsh, EFS_TXN_EXCL, t, parts, d->k_hash, d->kh,
                        d->hash_ver, EFS_TXN_PUT, d->v_tomb,
                        sizeof(d->v_tomb), hint);
    else if (d->del_hash && sh == d->hsh)
        rc = host_prep(h, d->hsh, EFS_TXN_EXCL, t, parts, d->k_hash, d->kh,
                        d->hash_ver, EFS_TXN_DEL, NULL, 0, hint);
    if (rc == EFS_OK && d->del_loc && sh == d->psh)
        rc = host_prep(h, d->psh, EFS_TXN_EXCL, t, parts, d->k_loc, d->kl,
                       d->loc_ver, EFS_TXN_DEL, NULL, 0, hint);
    return rc;
}

/* First use of a HASHED/SPLITTING dir lane whose dentry shard is on a
 * different Raft group than the parent inode. Parent used_shards is
 * exclusive; dest dentry, child inode, alloc, dseq, and dir-lane ride
 * the dest shard. Parent mtime/ctime stay on the dir-lane (not the
 * home row). */
static int host_hashed_create_txn(struct efs_raft_host *h, efs_ino_t parent,
                                  const char *name, uint32_t mode,
                                  const struct efs_meta_attrs *at,
                                  struct efs_meta_row *prow, uint32_t dsh,
                                  int *hint)
{
    struct efs_meta_row crow;
    struct efs_meta_dentry dent;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t k_ln[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_pino[EFS_META_INO_BYTES];
    uint8_t v_cino[EFS_META_INO_BYTES], v_alloc[EFS_META_ALLOC_BYTES];
    uint8_t v_dseq[8], v_ln[EFS_META_LANE_BYTES], sb[8];
    uint32_t kd = 0, kpi = 0, kci = 0, ka = 0, ks = 0, kln = 0, sn = 8;
    uint32_t psh, coord;
    uint64_t pver = 0, aver = 0, sver = 0, dver = 0, lver = 0, seq = 0;
    uint64_t bit;
    uint8_t lane, cmd[22];
    efs_ino_t next = 0, ino = 0;
    int rc, i, gr;

    psh = efs_kv_inode_shard(parent);
    lane = efs_kv_dir_lane(name);
    bit = 1ull << lane;
    prow->used_shards |= bit;
    rc = efs_meta_apply_peek_alloc(h->kv, dsh, &next);
    if (rc == EFS_OK) {
        ino = next;
        if (efs_kv_inode_shard(ino) != dsh)
            rc = EFS_ERR_PROTO;
        next = ino + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
    }
    if (rc == EFS_OK) {
        memset(&crow, 0, sizeof(crow));
        crow.ino = ino;
        crow.generation = 1;
        crow.mode = mode;
        crow.nlink = 1;
        crow.parent = parent;
        crow.uid = at->uid;
        crow.gid = at->gid;
        crow.base_mtime = at->now;
        crow.base_atime = at->now;
        crow.base_ctime = at->now;
        memset(&dent, 0, sizeof(dent));
        dent.ino = ino;
        dent.generation = 1;
        dent.type = mode & S_IFMT;
        rc = efs_meta_pack_dentry(&dent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(prow, v_pino, sizeof(v_pino));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&crow, v_cino, sizeof(v_cino));
        wr64be(v_alloc, next);
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(dsh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(dsh, ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(dsh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, lane, k_dseq, &ks);
    if (rc == EFS_OK)
        rc = efs_meta_stamp_dir_lane(h->kv, prow, name, at->now, k_ln, &kln,
                                     v_ln, sizeof(v_ln));
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_pino, kpi, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_alloc, ka, &aver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_ln, kln, &lver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        parts.n = 2;
        if (psh < dsh) {
            parts.shard[0] = psh;
            parts.shard[1] = dsh;
        } else {
            parts.shard[0] = dsh;
            parts.shard[1] = psh;
        }
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == psh)
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_pino, kpi,
                               pver, EFS_TXN_PUT, v_pino, sizeof(v_pino),
                               hint);
            if (rc == EFS_OK && sh == dsh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dent, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_cino,
                                   kci, 0, EFS_TXN_PUT, v_cino, sizeof(v_cino),
                                   hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_alloc,
                                   ka, aver, EFS_TXN_PUT, v_alloc,
                                   sizeof(v_alloc), hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dseq,
                                   ks, sver, EFS_TXN_PUT, v_dseq, 8, hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_ln, kln,
                                   lver, EFS_TXN_PUT, v_ln, sizeof(v_ln),
                                   hint);
            }
        }
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22,
                                   hint);
            for (i = 0; i < parts.n && rc == EFS_OK; i++) {
                pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
                rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]),
                                       cmd, 22, hint);
            }
        }
    }
    return rc;
}

/* MKDIR: child inode scatters to csh (mkdir shard), the dentry goes to
 * dsh = dentry_shard(parent, name, layout) — the parent's home shard for a
 * LOCAL dir, the hashed lane shard for HASHED/SPLITTING. Parent row carries
 * nlink++ always; times ride the parent row for LOCAL, the dir-lane stamp
 * (plus a first-use used_shards bit) for HASHED/SPLITTING. dseq bumps on
 * (dsh, lane). A node that does not host a participant group bounces. */
void server_raft_host_mkdir(efs_ino_t parent, const char *name, uint32_t mode,
                            uint32_t uid, uint32_t gid,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, crow;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[EFS_META_LANE_BYTES];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_pino[EFS_META_INO_BYTES];
    uint8_t v_cino[EFS_META_INO_BYTES], v_alloc[EFS_META_ALLOC_BYTES];
    uint8_t v_dseq[8], sb[8];
    uint32_t kd = 0, kpi = 0, kci = 0, ka = 0, ks = 0, kln = 0, sn = 8;
    uint32_t psh, csh, dsh, coord;
    uint64_t pver = 0, aver = 0, sver = 0, dver = 0, lnver = 0, salt = 0;
    uint64_t seq = 0, now;
    efs_ino_t next = 0, ino = 0;
    uint8_t cmd[22], p_lane = 0;
    int hint = -1;
    int rc, i, gr;
    int stage = 0, stamp_lane = 0;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0 || name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if ((mode & S_IFMT) != S_IFDIR)
        mode = S_IFDIR | (mode & 07777);
    if ((mode & 07777) == 0)
        mode |= 0755;
    now = now_ns();
    psh = efs_kv_inode_shard(parent);
    if (!host_hosts(h, efs_raft_shard_group(psh))) {
        uint8_t need[2];
        need[0] = EFS_RAFT_GROUP_SHARD;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_create(h, parent, name, mode, uid, gid, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    stage=1; rc = host_read_index(h, efs_raft_shard_group(efs_kv_inode_shard(EFS_ROOT_INO)),
                         &hint);
    if (rc == EFS_OK)
        stage=2; rc = efs_meta_apply_export_salt(h->kv, &salt);
    csh = (rc == EFS_OK) ? efs_kv_mkdir_shard(parent, name, salt) : 0;
    if (rc == EFS_OK && !host_hosts(h, efs_raft_shard_group(csh))) {
        uint8_t need[2];
        int nn = 1;
        need[0] = efs_raft_shard_group(psh);
        if (efs_raft_shard_group(csh) != need[0])
            need[nn++] = efs_raft_shard_group(csh);
        pthread_mutex_unlock(&h->read_mu);
        host_fwd_create(h, parent, name, mode, uid, gid, out, need, nn);
        return;
    }
    if (rc == EFS_OK)
        stage=3; rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK && efs_raft_shard_group(csh) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(csh), &hint);
    if (rc == EFS_OK)
        stage=4; rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL &&
        prow.layout != EFS_META_LAYOUT_HASHED &&
        prow.layout != EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        p_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0
                                                      : efs_kv_dir_lane(name);
        if (!host_hosts(h, efs_raft_shard_group(dsh))) {
            uint8_t need[3];
            int nn = 0;
            bounce_add(need, &nn, 3, efs_raft_shard_group(psh));
            bounce_add(need, &nn, 3, efs_raft_shard_group(csh));
            bounce_add(need, &nn, 3, efs_raft_shard_group(dsh));
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_create(h, parent, name, mode, uid, gid, out, need, nn);
            return;
        }
        if (efs_raft_shard_group(dsh) != efs_raft_shard_group(psh) &&
            efs_raft_shard_group(dsh) != efs_raft_shard_group(csh))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    } else
        dsh = 0;
    if (rc == EFS_OK)
        stage=5; rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK) {
        pthread_mutex_unlock(&h->read_mu);
        set_inode_rc(out, EFS_ERR_EXIST, hint);
        return;
    }
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    if (rc == EFS_OK)
        stage=6; rc = efs_meta_apply_peek_alloc(h->kv, csh, &next);
    if (rc == EFS_OK) {
        ino = next;
        if (efs_kv_inode_shard(ino) != csh)
            rc = EFS_ERR_PROTO;
        next = ino + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
    }
    if (rc == EFS_OK) {
        memset(&crow, 0, sizeof(crow));
        crow.ino = ino;
        crow.generation = 1;
        crow.mode = mode;
        crow.nlink = 2;
        crow.parent = parent;
        crow.uid = uid;
        crow.gid = gid;
        crow.base_mtime = now;
        crow.base_atime = now;
        crow.base_ctime = now;
        memset(&dent, 0, sizeof(dent));
        dent.ino = ino;
        dent.generation = 1;
        dent.type = S_IFDIR;
        prow.nlink++;
        if (prow.layout == EFS_META_LAYOUT_LOCAL) {
            if (prow.base_mtime < now)
                prow.base_mtime = now;
            if (prow.base_ctime < now)
                prow.base_ctime = now;
            efs_meta_dir_note_entry(&prow, 1);
        } else {
            /* Times live on the dir-lane (§7.4); the home row carries
             * nlink++ and the first-use used_shards bit only. */
            uint64_t bit = 1ull << efs_kv_dir_lane(name);
            if ((prow.used_shards & bit) == 0)
                prow.used_shards |= bit;
            stamp_lane = 1;
        }
        rc = efs_meta_pack_dentry(&dent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&prow, v_pino, sizeof(v_pino));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&crow, v_cino, sizeof(v_cino));
        wr64be(v_alloc, next);
    }
    if (rc == EFS_OK)
        stage=7; rc = efs_kv_key_dentry(dsh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(csh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, p_lane, k_dseq, &ks);
    if (rc == EFS_OK && stamp_lane)
        rc = efs_meta_stamp_dir_lane(h->kv, &prow, name, now, k_ln, &kln,
                                     v_ln, sizeof(v_ln));
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_pino, kpi, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_alloc, ka, &aver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc == EFS_OK && stamp_lane)
        rc = efs_txn_ver_get(h->kv, k_ln, kln, &lnver);
    if (rc == EFS_OK)
        stage=8; rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        rc = host_parts_add(&parts, psh);
        if (rc == EFS_OK)
            rc = host_parts_add(&parts, csh);
        if (rc == EFS_OK)
            rc = host_parts_add(&parts, dsh);
        if (rc != EFS_OK)
            goto mkdir_prepped;
        stage=9; fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == dsh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dent, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dseq,
                                   ks, sver, EFS_TXN_PUT, v_dseq, 8, &hint);
                if (rc == EFS_OK && stamp_lane)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_ln,
                                   kln, lnver, EFS_TXN_PUT, v_ln,
                                   sizeof(v_ln), &hint);
            }
            if (rc == EFS_OK && sh == psh)
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_pino,
                               kpi, pver, EFS_TXN_PUT, v_pino,
                               sizeof(v_pino), &hint);
            if (rc == EFS_OK && sh == csh) {
                rc = host_prep(h, csh, EFS_TXN_EXCL, &t, &parts, k_cino, kci,
                               0, EFS_TXN_PUT, v_cino, sizeof(v_cino),
                               &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, csh, EFS_TXN_EXCL, &t, &parts, k_alloc,
                                   ka, aver, EFS_TXN_PUT, v_alloc,
                                   sizeof(v_alloc), &hint);
            }
        }
mkdir_prepped:
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            stage=10; coord = efs_txn_coordinator(&t, &parts);
            pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22,
                                   &hint);
            for (i = 0; i < parts.n && rc == EFS_OK; i++) {
                pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
                rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]),
                                       cmd, 22, &hint);
            }
        }
    }
    stage=11;
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, dent.ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, dent.ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    if (rc != EFS_OK && env_on("EFS_RAFT_DBG"))
        fprintf(stderr, "raft-host: mkdir parent=%llu namelen=%zu rc=%d "
                "hint=%d psh=%u csh=%u dsh=%u stage=%d ino=%llu\n",
                (unsigned long long)parent, strlen(name), rc, hint, psh,
                csh, dsh, stage, (unsigned long long)ino);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* Empty RMDIR: parent dentry drop + parent nlink-- + child inode DEL.
 * LOCAL parent stamps the parent row; HASHED/SPLITTING stamp the
 * dir-lane. SPLITTING parent writes HASHED=TOMBSTONE (I8). HASHED
 * child GUARDs used-lane dseqs. A child that is itself SPLITTING stays
 * BUSY (distributed emptiness). A node that does not host a
 * participant group bounces. */
void server_raft_host_rmdir(efs_ino_t parent, const char *name,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    struct efs_meta_dir_cursor cur;
    struct efs_meta_dir_ent one;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct host_pver_guard gv[EFS_TXN_MAX_PART];
    struct host_dent_drop drop;
    uint8_t k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_pdseq[EFS_KV_KEY_MAX], k_cdseq[EFS_KV_KEY_MAX];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[EFS_META_LANE_BYTES];
    uint8_t v_pino[EFS_META_INO_BYTES], v_pdseq[8], sb[8];
    uint32_t kpi = 0, kci = 0, kps = 0, kcs = 0, kln = 0, sn = 8, nent = 0;
    uint32_t psh, csh, dsh, coord;
    uint64_t pver = 0, sver = 0, cver = 0, gver = 0, lnver = 0, seq = 0, now;
    uint8_t cmd[22], p_lane = 0;
    int hint = -1;
    int rc, i, gr, held, stamp_lane = 0, ngv = 0, hashed_child = 0;

    memset(out, 0, sizeof(*out));
    memset(&drop, 0, sizeof(drop));
    memset(&drop, 0, sizeof(drop));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0 || name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    now = now_ns();
    psh = efs_kv_inode_shard(parent);
    if (!host_hosts(h, efs_raft_shard_group(psh))) {
        uint8_t need[2];
        need[0] = EFS_RAFT_GROUP_SHARD;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_unlink(h, parent, name, 1, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL &&
        prow.layout != EFS_META_LAYOUT_HASHED &&
        prow.layout != EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        p_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
        if (!host_hosts(h, efs_raft_shard_group(dsh))) {
            uint8_t need[2];
            int nn = 1;
            need[0] = efs_raft_shard_group(psh);
            if (efs_raft_shard_group(dsh) != need[0])
                need[nn++] = efs_raft_shard_group(dsh);
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_unlink(h, parent, name, 1, out, need, nn);
            return;
        }
        if (efs_raft_shard_group(dsh) != efs_raft_shard_group(psh))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    } else
        dsh = 0;
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK && dent.ino == EFS_ROOT_INO)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && (dent.type & S_IFMT) != S_IFDIR)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        csh = efs_kv_inode_shard(dent.ino);
        if (!host_hosts(h, efs_raft_shard_group(csh))) {
            uint8_t need[3];
            int nn = 1;
            need[0] = efs_raft_shard_group(psh);
            bounce_add(need, &nn, 3, efs_raft_shard_group(dsh));
            bounce_add(need, &nn, 3, efs_raft_shard_group(csh));
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_unlink(h, parent, name, 1, out, need, nn);
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_resolve(h->kv, parent, name, &dent, &row);
    if (rc == EFS_OK && (!S_ISDIR(row.mode) || row.ino == EFS_ROOT_INO))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && row.layout == EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_BUSY;
    else if (rc == EFS_OK && row.layout != EFS_META_LAYOUT_LOCAL &&
             row.layout != EFS_META_LAYOUT_HASHED)
        rc = EFS_ERR_INVAL;
    csh = (rc == EFS_OK) ? efs_kv_inode_shard(row.ino) : 0;
    if (rc == EFS_OK && row.layout == EFS_META_LAYOUT_HASHED) {
        uint8_t gs[8];
        int ngs = 0, lane;
        hashed_child = 1;
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(psh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(dsh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(csh));
        for (lane = 0; lane < EFS_META_LANES; lane++) {
            if ((row.used_shards & (1ull << lane)) == 0)
                continue;
            bounce_add(gs, &ngs, 8,
                       efs_raft_shard_group(efs_kv_lane_shard(row.ino, (uint8_t)lane)));
        }
        if (!host_hosts_all(h, gs, ngs)) {
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_unlink(h, parent, name, 1, out, gs, ngs);
            return;
        }
        rc = host_read_inode_lanes(h, row.ino, &hint);
    } else if (rc == EFS_OK && efs_raft_shard_group(csh) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(csh), &hint);
    if (rc == EFS_OK && prow.nlink < 3)
        rc = EFS_ERR_PROTO;
    if (rc == EFS_OK && row.nlink > 2)
        rc = EFS_ERR_NOT_EMPTY;
    if (rc == EFS_OK) {
        memset(&cur, 0, sizeof(cur));
        memset(&one, 0, sizeof(one));
        nent = 0;
        rc = efs_meta_apply_readdir(h->kv, row.ino, &cur, &one, 1, &nent);
        if (rc == EFS_OK && nent > 0)
            rc = EFS_ERR_NOT_EMPTY;
    }
    if (rc == EFS_OK) {
        held = efs_lease_any(h->kv, row.ino, row.generation);
        if (held < 0)
            rc = held;
        else if (held)
            rc = EFS_ERR_BUSY;
    }
    if (rc == EFS_OK && hashed_child) {
        uint8_t lane;
        for (lane = 0; lane < EFS_META_LANES && rc == EFS_OK; lane++) {
            uint32_t lsh;
            if ((row.used_shards & (1ull << lane)) == 0)
                continue;
            if (ngv >= EFS_TXN_MAX_PART) {
                rc = EFS_ERR_BUSY;
                break;
            }
            lsh = efs_kv_lane_shard(row.ino, (uint8_t)lane);
            gv[ngv].shard = lsh;
            gv[ngv].klen = 0;
            rc = efs_kv_key_dseq(lsh, row.ino, (uint8_t)lane, gv[ngv].key,
                                  &gv[ngv].klen);
            if (rc == EFS_OK)
                rc = efs_txn_ver_get(h->kv, gv[ngv].key, gv[ngv].klen,
                                      &gv[ngv].ver);
            if (rc == EFS_OK)
                ngv++;
        }
    }
    if (rc == EFS_OK) {
        prow.nlink--;
        if (prow.layout == EFS_META_LAYOUT_LOCAL) {
            if (prow.base_mtime < now)
                prow.base_mtime = now;
            if (prow.base_ctime < now)
                prow.base_ctime = now;
            efs_meta_dir_note_entry(&prow, -1);
        } else
            stamp_lane = 1;
        rc = efs_meta_pack_inode(&prow, v_pino, sizeof(v_pino));
    }
    if (rc == EFS_OK)
        rc = host_dent_drop_fill(h->kv, &prow, parent, name, &drop);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, row.ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, p_lane, k_pdseq, &kps);
    if (rc == EFS_OK && !hashed_child)
        rc = efs_kv_key_dseq(csh, row.ino, 0, k_cdseq, &kcs);
    if (rc == EFS_OK && stamp_lane)
        rc = efs_meta_stamp_dir_lane(h->kv, &prow, name, now, k_ln, &kln, v_ln,
                                     sizeof(v_ln));
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_pino, kpi, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_pdseq, kps, &sver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_cino, kci, &cver);
    if (rc == EFS_OK && !hashed_child)
        rc = efs_txn_ver_get(h->kv, k_cdseq, kcs, &gver);
    if (rc == EFS_OK && stamp_lane)
        rc = efs_txn_ver_get(h->kv, k_ln, kln, &lnver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_pdseq, kps, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_pdseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        rc = host_parts_add(&parts, dsh);
        if (rc == EFS_OK)
            rc = host_parts_add(&parts, psh);
        if (rc == EFS_OK && drop.del_loc)
            rc = host_parts_add(&parts, drop.psh);
        if (rc == EFS_OK && (drop.put_tomb || drop.del_hash))
            rc = host_parts_add(&parts, drop.hsh);
        if (rc == EFS_OK)
            rc = host_parts_add(&parts, csh);
        for (i = 0; i < ngv && rc == EFS_OK; i++)
            rc = host_parts_add(&parts, gv[i].shard);
        if (rc != EFS_OK)
            goto rmdir_prepped;
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            int g;
            rc = host_dent_drop_prep(h, sh, &drop, &t, &parts, &hint);
            if (rc == EFS_OK && sh == dsh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_pdseq,
                                   kps, sver, EFS_TXN_PUT, v_pdseq, 8, &hint);
                if (rc == EFS_OK && stamp_lane)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_ln, kln,
                                   lnver, EFS_TXN_PUT, v_ln, sizeof(v_ln),
                                   &hint);
            }
            if (rc == EFS_OK && sh == psh)
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_pino, kpi,
                               pver, EFS_TXN_PUT, v_pino, sizeof(v_pino),
                               &hint);
            if (rc == EFS_OK && sh == csh) {
                rc = host_prep(h, csh, EFS_TXN_EXCL, &t, &parts, k_cino, kci,
                               cver, EFS_TXN_DEL, NULL, 0, &hint);
                if (rc == EFS_OK && !hashed_child)
                    rc = host_prep(h, csh, EFS_TXN_GUARD, &t, &parts, k_cdseq,
                                   kcs, gver, 0, NULL, 0, &hint);
            }
            for (g = 0; g < ngv && rc == EFS_OK; g++) {
                if (gv[g].shard != sh)
                    continue;
                rc = host_prep(h, gv[g].shard, EFS_TXN_GUARD, &t, &parts,
                               gv[g].key, gv[g].klen, gv[g].ver, 0, NULL, 0,
                               &hint);
            }
        }
rmdir_prepped:
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22,
                                   &hint);
            for (i = 0; i < parts.n && rc == EFS_OK; i++) {
                pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
                rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]),
                                       cmd, 22, &hint);
            }
        }
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        out->inode.ino = row.ino;
        out->inode.mode = row.mode;
        out->inode.nlink = 2;
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* nlink>1 file UNLINK, or last-link when dentry shard ≠ inode shard:
 * dest dentry drop + inode nlink-- (or inode DEL) as a txn (same
 * PREPARE/DECIDE/RESOLVE as LINK). LOCAL stamps the parent row;
 * HASHED/SPLITTING stamp the dir-lane (parent nlink unchanged).
 * SPLITTING writes HASHED=TOMBSTONE (I8) and DELs the local leftover.
 * Last-link on one group stays on EFS_MD_CMD_UNLINK unless a SPLITTING
 * leftover lives on another group. The receiving node must lead every
 * participant group. */
static void host_unlink_txn(efs_ino_t parent, const char *name,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct host_dent_drop drop;
    uint8_t k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX], v_ln[EFS_META_LANE_BYTES];
    uint8_t v_ino[EFS_META_INO_BYTES], v_par[EFS_META_INO_BYTES], v_dseq[8];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint8_t sb[8], cmd[22];
    uint32_t ki = 0, kp = 0, ks = 0, kln = 0, sn = 8, krl = 0;
    uint32_t dsh, ish, psh, coord, ash = 0;
    uint64_t iver = 0, pver = 0, sver = 0, lnver = 0, seq = 0, now;
    uint64_t rver = 0;
    uint32_t nlink_out = 0;
    int hint = -1;
    int rc, i, gr, held = 0, last = 0, put_ino = 0;
    int touch_parent = 0, stamp_lane = 0;
    uint8_t d_lane = 0;

    memset(out, 0, sizeof(*out));
    memset(&drop, 0, sizeof(drop));
    memset(&drop, 0, sizeof(drop));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0 || name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    now = now_ns();
    pthread_mutex_lock(&h->read_mu);
    psh = efs_kv_inode_shard(parent);
    rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL &&
        prow.layout != EFS_META_LAYOUT_HASHED &&
        prow.layout != EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        if (efs_raft_shard_group(dsh) != efs_raft_shard_group(psh))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    } else
        dsh = 0;
    if (rc == EFS_OK)
        rc = efs_meta_apply_resolve(h->kv, parent, name, &dent, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    ish = (rc == EFS_OK) ? efs_kv_inode_shard(row.ino) : 0;
    if (rc == EFS_OK && efs_raft_shard_group(ish) != efs_raft_shard_group(dsh))
        rc = host_read_index(h, efs_raft_shard_group(ish), &hint);
    if (rc == EFS_OK) {
        nlink_out = row.nlink;
        last = row.nlink <= 1;
        if (last) {
            held = efs_lease_any(h->kv, row.ino, row.generation);
            if (held < 0)
                rc = held;
            else if (held) {
                row.nlink = 0;
                if (row.base_ctime < now)
                    row.base_ctime = now;
                put_ino = 1;
            }
        } else {
            row.nlink--;
            if (row.base_ctime < now)
                row.base_ctime = now;
            put_ino = 1;
        }
    }
    if (rc == EFS_OK) {
        d_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
        if (prow.layout == EFS_META_LAYOUT_LOCAL) {
            if (prow.base_mtime < now)
                prow.base_mtime = now;
            if (prow.base_ctime < now)
                prow.base_ctime = now;
            efs_meta_dir_note_entry(&prow, -1);
            touch_parent = 1;
        } else
            stamp_lane = 1;
        if (touch_parent)
            rc = efs_meta_pack_inode(&prow, v_par, sizeof(v_par));
        if (rc == EFS_OK && put_ino)
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
    }
    if (rc == EFS_OK)
        rc = host_dent_drop_fill(h->kv, &prow, parent, name, &drop);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, row.ino, k_ino, &ki);
    if (rc == EFS_OK && touch_parent)
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, d_lane, k_dseq, &ks);
    if (rc == EFS_OK && stamp_lane)
        rc = efs_meta_stamp_dir_lane(h->kv, &prow, name, now, k_ln, &kln, v_ln,
                                     sizeof(v_ln));
    /* Last link with no leases retires the inode row: the reap marker
     * (L7) rides the same txn on the inode group's anchor shard. */
    if (rc == EFS_OK && last && !held) {
        ash = efs_kv_anchor_shard(ish);
        rc = efs_kv_key_reap(ash, row.ino, k_reap, &krl);
        if (rc == EFS_OK)
            rc = efs_txn_ver_get(h->kv, k_reap, krl, &rver);
        if (rc == EFS_OK)
            efs_meta_pack_reap(v_reap, row.generation, row.active_lanes);
    }
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_ino, ki, &iver);
    if (rc == EFS_OK && touch_parent)
        rc = efs_txn_ver_get(h->kv, k_par, kp, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc == EFS_OK && stamp_lane)
        rc = efs_txn_ver_get(h->kv, k_ln, kln, &lnver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        rc = host_parts_add(&parts, dsh);
        if (rc == EFS_OK)
            rc = host_parts_add(&parts, ish);
        if (rc == EFS_OK && touch_parent)
            rc = host_parts_add(&parts, psh);
        if (rc == EFS_OK && drop.del_loc)
            rc = host_parts_add(&parts, drop.psh);
        if (rc == EFS_OK && (drop.put_tomb || drop.del_hash))
            rc = host_parts_add(&parts, drop.hsh);
        if (rc == EFS_OK && last && !held)
            rc = host_parts_add(&parts, ash);
        if (rc != EFS_OK)
            goto prepped;
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            rc = host_dent_drop_prep(h, sh, &drop, &t, &parts, &hint);
            if (rc == EFS_OK && sh == dsh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dseq, ks,
                               sver, EFS_TXN_PUT, v_dseq, 8, &hint);
                if (rc == EFS_OK && stamp_lane)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_ln, kln,
                                   lnver, EFS_TXN_PUT, v_ln, sizeof(v_ln),
                                   &hint);
            }
            if (rc == EFS_OK && touch_parent && sh == psh)
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_par, kp,
                               pver, EFS_TXN_PUT, v_par, sizeof(v_par), &hint);
            if (rc == EFS_OK && sh == ish) {
                if (put_ino)
                    rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                                   iver, EFS_TXN_PUT, v_ino, sizeof(v_ino),
                                   &hint);
                else
                    rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                                   iver, EFS_TXN_DEL, NULL, 0, &hint);
            }
            if (rc == EFS_OK && last && !held && sh == ash && ash != dsh &&
                ash != ish && (!touch_parent || ash != psh) &&
                (!drop.del_loc || ash != drop.psh))
                rc = host_prep(h, ash, EFS_TXN_EXCL, &t, &parts, k_reap, krl,
                               rver, EFS_TXN_PUT, v_reap, sizeof(v_reap),
                               &hint);
        }
        if (rc == EFS_OK && last && !held &&
            (ash == dsh || ash == ish || (touch_parent && ash == psh) ||
             (drop.del_loc && ash == drop.psh)))
            /* The anchor shard is already a participant; its marker PREP
             * still has to ride the log of ITS OWN group. When ash aliases
             * another participant the loop above skipped the dedicated
             * branch, so issue it here against the anchor's group. */
            rc = host_prep(h, ash, EFS_TXN_EXCL, &t, &parts, k_reap, krl,
                           rver, EFS_TXN_PUT, v_reap, sizeof(v_reap),
                           &hint);
prepped:
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22,
                                   &hint);
            for (i = 0; i < parts.n && rc == EFS_OK; i++) {
                pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
                rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]),
                                       cmd, 22, &hint);
            }
        }
    }
    if (rc == EFS_OK && !last) {
        rc = host_read_inode_lanes(h, row.ino, &hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_getattr(h->kv, row.ino, host_txn_coord, h, &st);
    }
    if (rc != EFS_OK && env_on("EFS_RAFT_DBG"))
        fprintf(stderr,
                "raft-host: unlink-txn parent=%llu name=%s rc=%d hint=%d "
                "last=%d held=%d dsh=%u ish=%u psh=%u\n",
                (unsigned long long)parent, name, rc, hint, last, held,
                dsh, ish, psh);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        if (!last)
            stat_to_inode(&st, &out->inode);
        else {
            out->inode.ino = row.ino;
            out->inode.mode = row.mode;
            out->inode.nlink = nlink_out;
        }
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* Last-link file UNLINK on one group: one Raft entry on the dentry
 * group. nlink>1, last-link with dsh ≠ ish, or SPLITTING unlink whose
 * local leftover is on another group, is the txn above.
 * Directories go through RMDIR. */
void server_raft_host_unlink(efs_ino_t parent, const char *name, int is_dir,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t clen = 0;
    uint64_t idx = 0;
    uint32_t dsh = 0;
    uint8_t pg, dg = 0, ig;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (is_dir) {
        server_raft_host_rmdir(parent, name, out);
        return;
    }
    /* The unlink may need the inode's group as well as the parent/dentry
     * group (last-link frees the inode row). Forward to a host that has
     * every group this op can touch rather than failing ReadIndex with an
     * unactionable NOT_PRIMARY. */
    if (!host_hosts(h, efs_raft_shard_group(efs_kv_inode_shard(parent)))) {
        uint8_t need[2];
        host_need_both(need);
        host_fwd_unlink(h, parent, name, 0, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(efs_kv_inode_shard(parent)),
                         &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        dg = efs_raft_shard_group(dsh);
        pg = efs_raft_shard_group(efs_kv_inode_shard(parent));
        if (!host_hosts(h, dg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            if (dg != pg)
                need[nn++] = dg;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_unlink(h, parent, name, 0, out, need, nn);
            return;
        }
        if (dg != pg) {
            int hh = -1;
            rc = host_read_index(h, dg, &hh);
            if (rc != EFS_OK)
                hint = hh;
        }
    }
    if (rc == EFS_OK) {
        /* Dentry-first: the inode row can live on a group this host does
         * not host, and resolve would then fail I9 (EFS_ERR_IO) before the
         * hosting check below ever runs. Look the dentry up locally, bounce
         * to a host that has the inode's group, and only then resolve. */
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
        if (rc == EFS_OK && (dent.type & S_IFMT) == S_IFDIR) {
            pthread_mutex_unlock(&h->read_mu);
            server_raft_host_rmdir(parent, name, out);
            return;
        }
        if (rc == EFS_OK) {
            ig = efs_raft_shard_group(efs_kv_inode_shard(dent.ino));
            if (!host_hosts(h, ig)) {
                uint8_t need[2];
                int nn = 1;
                need[0] = dg;
                if (ig != dg)
                    need[nn++] = ig;
                pthread_mutex_unlock(&h->read_mu);
                host_fwd_unlink(h, parent, name, 0, out, need, nn);
                return;
            }
        }
        if (rc == EFS_OK)
            rc = efs_meta_apply_resolve(h->kv, parent, name, &dent, &row);
        if (rc == EFS_OK) {
            if (row.nlink > 1 || ig != dg ||
                (prow.layout == EFS_META_LAYOUT_SPLITTING && pg != dg)) {
                pthread_mutex_unlock(&h->read_mu);
                host_unlink_txn(parent, name, out);
                return;
            }
        }
    }
    if (rc == EFS_OK)
        rc = pack_unlink_cmd(cmd, &clen, parent, now_ns(), name);
    if (rc == EFS_OK)
        rc = host_propose(h, dg, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, dg, idx, &hint);
    if (rc != EFS_OK && env_on("EFS_RAFT_DBG"))
        fprintf(stderr,
                "raft-host: unlink-simple parent=%llu name=%s rc=%d hint=%d "
                "dsh=%u dg=%u\n",
                (unsigned long long)parent, name, rc, hint, dsh, dg);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        out->inode.ino = row.ino;
        out->inode.mode = row.mode;
        out->inode.nlink = row.nlink;
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* Mode/owner SETATTR: one Raft entry on the inode shard. SIZE is INVAL
 * (truncate later). MTIME/ATIME is the utimens fence (below). Mixed
 * mode+time classes are INVAL — the §6 matrix splits them. */
static void host_utimens(efs_ino_t ino, uint32_t mask, uint64_t mtime,
                         uint32_t mtime_nsec, uint64_t atime,
                         struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_utimens u;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    uint8_t cmd[HOST_UTIMENS_LEN];
    uint32_t clen = 0;
    uint64_t idx = 0, bits;
    uint8_t g;
    int hint = -1;
    int rc;
    uint32_t i;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if ((mask & (EFS_SETATTR_MTIME | EFS_SETATTR_ATIME)) == 0 ||
        (mask & ~(EFS_SETATTR_MTIME | EFS_SETATTR_ATIME)) != 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    memset(&u, 0, sizeof(u));
    if (mask & EFS_SETATTR_MTIME) {
        u.mask |= EFS_META_SET_MTIME;
        u.mtime = mtime * 1000000000ull + (uint64_t)mtime_nsec;
    }
    if (mask & EFS_SETATTR_ATIME) {
        u.mask |= EFS_META_SET_ATIME;
        u.atime = atime * 1000000000ull;
    }
    u.expect_gen = 0;
    g = efs_raft_shard_group(efs_kv_inode_shard(ino));
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, g, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && (u.mask & EFS_META_SET_MTIME)) {
        u.mtime_gen = row.mtime_gen + 1;
        bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
                   ? row.used_shards
                   : row.active_lanes;
        for (i = 0; i < EFS_META_LANES && rc == EFS_OK; i++) {
            uint32_t lsh;
            uint8_t lg;

            if ((bits & (1ULL << i)) == 0)
                continue;
            lsh = efs_kv_lane_shard(ino, (uint8_t)i);
            lg = efs_raft_shard_group(lsh);
            if (lg != g)
                rc = EFS_ERR_INVAL; /* cross-group lane fence later */
        }
    }
    if (rc == EFS_OK)
        rc = pack_utimens_cmd(cmd, &clen, ino, now_ns(), &u);
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, g, idx, &hint);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

/* SETATTR SIZE: content_epoch fence + base_size. Unaligned sizes mint a
 * same-group tail candidate (CAS inside the truncate entry). mtime is the
 * client-stamped time when the caller sent SIZE|MTIME (the client's truncate
 * always does — it marks the truncate newer than any in-flight REPORT), or
 * 0 to stamp now. */
static void host_truncate(efs_ino_t ino, uint64_t size, uint64_t mtime,
                          uint32_t mtime_nsec,
                          struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    struct efs_meta_pub tail;
    struct efs_meta_chunk got;
    const struct efs_meta_pub *tp = NULL;
    uint8_t cmd[HOST_CMD_MAX];
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint8_t cross[EFS_META_LANES];
    uint32_t clen = 0, tci = 0, lsh = 0;
    uint64_t idx = 0, bits = 0, now, same_mask = 0;
    uint8_t g, lane = 0, lg = 0, has_tail = 0;
    int hint = -1, ncross = 0, tail_ext = 0;
    int rc;
    uint32_t i;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    g = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, g)) {
        /* The truncate needs every active lane's group local (fences wait
         * on local apply); bounce to a dual-host. */
        uint8_t need[2];
        host_need_both(need);
        host_fwd_setattr(h, ino, EFS_SETATTR_SIZE | EFS_SETATTR_MTIME, 0, 0,
                         0, size, mtime, mtime_nsec, 0, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, g, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && !S_ISREG(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        /* Split the active lanes into the ones this group's KV holds
         * (fenced + range-deleted by the truncate entry itself) and the
         * cross-group ones (fenced by LANE_FENCE entries on their own
         * groups, proposed first). A group's log only writes its own
         * shards' keys. */
        bits = row.active_lanes;
        for (i = 0; i < EFS_META_LANES; i++) {
            if ((bits & (1ULL << i)) == 0)
                continue;
            lsh = efs_kv_lane_shard(ino, (uint8_t)i);
            lg = efs_raft_shard_group(lsh);
            if (lg == g)
                same_mask |= 1ULL << i;
            else
                cross[ncross++] = (uint8_t)i;
        }
        if (ncross && !host_hosts(h, efs_raft_shard_group(
                          efs_kv_lane_shard(ino, cross[0])))) {
            uint8_t need[2];
            pthread_mutex_unlock(&h->read_mu);
            host_need_both(need);
            host_fwd_setattr(h, ino, EFS_SETATTR_SIZE | EFS_SETATTR_MTIME, 0,
                             0, 0, size, mtime, mtime_nsec, 0, out, need, 2);
            return;
        }
    }
    if (rc == EFS_OK && size > 0 && (size % EFS_MIN_CHUNK_SIZE) != 0) {
        tci = (uint32_t)(size / EFS_MIN_CHUNK_SIZE);
        lane = (uint8_t)(tci % EFS_META_LANES);
        lsh = efs_kv_lane_shard(ino, lane);
        lg = efs_raft_shard_group(lsh);
        has_tail = 1;
        tail_ext = (lg != g);
        if (!tail_ext) {
            memset(&got, 0, sizeof(got));
            rc = efs_meta_apply_get_chunk(h->kv, ino, tci, &got);
            if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
        }
        if (rc == EFS_OK && !tail_ext) {
            memset(&tail, 0, sizeof(tail));
            memset(uuid, 0, sizeof(uuid));
            tail.ino = ino;
            tail.chunk_index = tci;
            tail.new_size = size;
            tail.expected_gen = got.generation;
            tail.candidate_gen = efs_meta_candidate_gen(uuid, 0, 2, tci, 0);
            if (tail.candidate_gen == 0 ||
                tail.candidate_gen == tail.expected_gen)
                tail.candidate_gen = tail.expected_gen + 1;
            if (tail.candidate_gen == 0)
                tail.candidate_gen = 1;
            tail.coding_profile_id = EFS_META_PROFILE_K2F1;
            if (got.generation != 0) {
                /* The tail stub writes no data of its own: alias the
                 * superseded row's placement so reads keep finding the
                 * surviving prefix's fragments. The apply side recognizes
                 * the alias and skips the GC record — the fragments are
                 * shared with the live row, not dead. */
                memcpy(tail.ch.nodes, got.nodes, sizeof(tail.ch.nodes));
                memcpy(tail.ch.checksums, got.checksums,
                       sizeof(tail.ch.checksums));
            } else {
                /* Grow into a chunk that was never written: publish the
                 * well-known zero-fragment digests so the read path
                 * synthesizes zeros without a GET (no fragments exist). */
                for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                    tail.ch.nodes[i] = (efs_node_id_t)(i + 1);
                    efs_hash_zero_fragment(tail.ch.checksums[i]);
                }
            }
            tp = &tail;
        }
    }
    /* Fence the cross-group lanes BEFORE the inode-group entry commits the
     * new epoch: a lane whose fenced_epoch already moved rejects any
     * stale-epoch publish, so no publication of the old epoch can slip in
     * after the row says the epoch advanced. Idempotent on retry — the
     * row's content_epoch only moves with the truncate entry, so a retried
     * truncate re-fences at the same new_epoch (a no-op). */
    if (rc == EFS_OK && ncross) {
        uint64_t new_epoch = row.content_epoch + 1;
        int k;

        for (k = 0; k < ncross && rc == EFS_OK; k++) {
            uint8_t fcmd[HOST_LANE_FENCE_LEN];
            uint32_t flen = 0;
            uint8_t fg = efs_raft_shard_group(efs_kv_lane_shard(ino, cross[k]));

            rc = pack_lane_fence_cmd(fcmd, &flen, ino, row.generation,
                                     cross[k], new_epoch, size, tci,
                                     has_tail);
            if (rc == EFS_OK)
                rc = host_propose_wait(h, fg, fcmd, flen, &hint);
        }
    }
    now = now_ns();
    if (mtime)
        now = mtime * 1000000000ull + (uint64_t)mtime_nsec;
    if (rc == EFS_OK)
        rc = pack_truncate_cmd(cmd, &clen, ino, now, 0, size, tp, same_mask,
                               (uint8_t)tail_ext);
    if (rc == EFS_OK && clen > HOST_CMD_MAX)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, g, idx, &hint);
    /* Cross-group tail: the fence on its lane already kept tail_ci and
     * rejects old-epoch publishes, so the zero-filled tail candidate goes
     * up as a lane-local publish under the NEW epoch after the commit.
     * The CAS base is re-read post-fence; a same-chunk writer that raced
     * in under the new epoch wins the CAS and this publish retries against
     * its generation — after HOST_PUB_TAIL_TRIES the truncate is still
     * committed (the entry landed); the tail stub losing that race is a
     * known slice gap (the raft path's tail zero-fill is a metadata stub
     * regardless), logged, not silently dropped. */
    if (rc == EFS_OK && tail_ext) {
        int attempt;

        for (attempt = 0; attempt < HOST_PUB_TAIL_TRIES; attempt++) {
            uint8_t tg = efs_raft_shard_group(efs_kv_lane_shard(ino, lane));

            rc = host_read_index(h, tg, &hint);
            if (rc == EFS_OK) {
                memset(&got, 0, sizeof(got));
                rc = efs_meta_apply_get_chunk(h->kv, ino, tci, &got);
                if (rc == EFS_ERR_NOT_FOUND)
                    rc = EFS_OK;
            }
            if (rc != EFS_OK)
                break;
            memset(&tail, 0, sizeof(tail));
            memset(uuid, 0, sizeof(uuid));
            tail.ino = ino;
            tail.chunk_index = tci;
            tail.new_size = size;
            tail.expected_gen = got.generation;
            tail.candidate_gen = efs_meta_candidate_gen(uuid, 0, 2, tci,
                                                        (uint32_t)attempt);
            if (tail.candidate_gen == 0 ||
                tail.candidate_gen == tail.expected_gen)
                tail.candidate_gen = tail.expected_gen + 1;
            if (tail.candidate_gen == 0)
                tail.candidate_gen = 1;
            tail.coding_profile_id = EFS_META_PROFILE_K2F1;
            tail.content_epoch = row.content_epoch + 1;
            tail.inode_gen = row.generation;
            tail.mtime_gen = row.mtime_gen;
            tail.lane_local = 1;
            if (got.generation != 0) {
                /* Alias the superseded row's placement (see the
                 * same-group branch above). */
                memcpy(tail.ch.nodes, got.nodes, sizeof(tail.ch.nodes));
                memcpy(tail.ch.checksums, got.checksums,
                       sizeof(tail.ch.checksums));
            } else {
                for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                    tail.ch.nodes[i] = (efs_node_id_t)(i + 1);
                    efs_hash_zero_fragment(tail.ch.checksums[i]);
                }
            }
            rc = pack_publish_cmd(cmd, &clen, &tail);
            if (rc == EFS_OK && clen > HOST_CMD_MAX)
                rc = EFS_ERR_INVAL;
            if (rc == EFS_OK)
                rc = host_propose(h, tg, cmd, clen, &idx, &hint);
            if (rc == EFS_OK)
                rc = host_wait_applied(h, tg, idx, &hint);
            if (rc != EFS_ERR_STALE)
                break;
        }
        if (rc == EFS_ERR_STALE) {
            fprintf(stderr,
                    "raft-host: truncate ino=%llu tail ci=%u lost CAS race; "
                    "committed anyway (tail stub gap)\n",
                    (unsigned long long)ino, tci);
            rc = EFS_OK;
        }
    }
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

void server_raft_host_setattr(efs_ino_t ino, uint32_t mask, uint32_t mode,
                              uint32_t uid, uint32_t gid, uint64_t size,
                              uint64_t mtime, uint32_t mtime_nsec,
                              uint64_t atime, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_setattr sa;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    uint8_t cmd[HOST_SETATTR_LEN];
    uint32_t clen = 0;
    uint64_t idx = 0;
    uint8_t g;
    int hint = -1;
    int rc;
    uint32_t own, times, sz;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    own = mask & (EFS_SETATTR_MODE | EFS_SETATTR_UID | EFS_SETATTR_GID);
    times = mask & (EFS_SETATTR_MTIME | EFS_SETATTR_ATIME);
    sz = mask & EFS_SETATTR_SIZE;
    if (sz) {
        /* Truncate. The client always sends SIZE|MTIME (the mtime marks the
         * truncate newer than any in-flight REPORT), so accept that pairing;
         * the stamped mtime rides the truncate entry. ATIME is meaningless
         * with SIZE. */
        if (own || (mask & EFS_SETATTR_ATIME) ||
            (times & ~EFS_SETATTR_MTIME) != 0) {
            out->status = EFS_INODE_RPC_INVAL;
            return;
        }
        host_truncate(ino, size, mtime, mtime_nsec, out);
        return;
    }
    if ((own && times)) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (times) {
        host_utimens(ino, times, mtime, mtime_nsec, atime, out);
        return;
    }
    if (own == 0 || (mask & ~(EFS_SETATTR_MODE | EFS_SETATTR_UID |
                              EFS_SETATTR_GID)) != 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.mask = mask & (EFS_META_SET_MODE | EFS_META_SET_UID | EFS_META_SET_GID);
    sa.mode = mode;
    sa.uid = uid;
    sa.gid = gid;
    sa.expect_gen = 0;
    rc = pack_setattr_cmd(cmd, &clen, ino, now_ns(), &sa);
    if (rc != EFS_OK) {
        set_inode_rc(out, rc, -1);
        return;
    }
    g = efs_raft_shard_group(efs_kv_inode_shard(ino));
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, g, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, g, idx, &hint);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

/* O_APPEND reserve. Reply size is the watermark (off+len). Visible
 * getattr size stays the frontier until REPORT resolves the rsv.
 * Optional sess_uuid tags the reservation so a later fence resolves it
 * as FENCED_HOLE (10.5c-35d). Absent keeps the zero-UUID stand-in. */
void server_raft_host_append(efs_ino_t ino, uint64_t len,
                             const uint8_t *sess_uuid, uint32_t sess_epoch,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_stat st;
    uint8_t cmd[HOST_APPEND_RSV_LEN];
    uint32_t clen = 0, nopen = 0;
    uint64_t idx = 0, wm = 0;
    uint8_t ig;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0 || len == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, ig)) {
        host_fwd_append(h, ino, len, sess_uuid, sess_epoch, out, &ig, 1);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && !S_ISREG(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = host_sess_gate(h, ino, sess_uuid, sess_epoch);
    if (rc == EFS_OK)
        rc = pack_append_rsv_cmd(cmd, &clen, ino, len, sess_uuid, sess_epoch);
    if (rc == EFS_OK)
        rc = host_propose(h, ig, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, ig, idx, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_append_state(h->kv, ino, &wm, NULL, &nopen);
    if (rc == EFS_OK && nopen == 0)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.size = wm;
    }
}

/* Hard link: dest dentry + inode nlink++ as a txn (same PREPARE/DECIDE/
 * RESOLVE as MKDIR). LOCAL dest stamps the parent row; HASHED/SPLITTING
 * stamp the dir-lane (parent row only for used_shards first-use).
 * SPLITTING dest writes hashed (same as CREATE). HASHED/SPLITTING dest
 * dentries bounce if this replica does not host the dentry shard.
 * Directories are INVAL. LINK_SHARD is not this path. */
void server_raft_host_link(efs_ino_t src_ino, efs_ino_t new_parent,
                           const char *new_name, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row dprow, row;
    struct efs_meta_dentry dent, ndent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX], v_ln[EFS_META_LANE_BYTES];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_ino[EFS_META_INO_BYTES];
    uint8_t v_par[EFS_META_INO_BYTES], v_dseq[8], sb[8], cmd[22];
    uint32_t kd = 0, ki = 0, kp = 0, ks = 0, kln = 0, sn = 8;
    uint32_t dsh, ish, psh, coord;
    uint64_t dver = 0, iver = 0, pver = 0, sver = 0, lnver = 0, seq = 0, now;
    int hint = -1;
    int rc, i, gr, touch_parent = 0, stamp_lane = 0;
    uint8_t d_lane = 0;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !new_name || src_ino == 0 || new_parent == 0 ||
        new_name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    now = now_ns();
    psh = efs_kv_inode_shard(new_parent);
    ish = efs_kv_inode_shard(src_ino);
    if (!host_hosts(h, efs_raft_shard_group(psh)) ||
        !host_hosts(h, efs_raft_shard_group(ish))) {
        uint8_t need[2];
        int nn = 1;
        need[0] = efs_raft_shard_group(psh);
        if (efs_raft_shard_group(ish) != need[0])
            need[nn++] = efs_raft_shard_group(ish);
        host_fwd_link(h, src_ino, new_parent, new_name, out, need, nn);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK && efs_raft_shard_group(ish) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(ish), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, new_parent, &dprow);
    if (rc == EFS_OK && !S_ISDIR(dprow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && dprow.layout != EFS_META_LAYOUT_LOCAL &&
        dprow.layout != EFS_META_LAYOUT_HASHED &&
        dprow.layout != EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, src_ino, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    dsh = (rc == EFS_OK)
              ? efs_kv_dentry_shard(new_parent, new_name, dprow.layout)
              : psh;
    if (rc == EFS_OK) {
        uint8_t gs[8];
        int ngs = 0;

        bounce_add(gs, &ngs, 8, efs_raft_shard_group(psh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(ish));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(dsh));
        if (!host_hosts_all(h, gs, ngs)) {
            uint8_t need[2];

            host_need_both(need);
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_link(h, src_ino, new_parent, new_name, out, need, 2);
            return;
        }
        if (efs_raft_shard_group(dsh) != efs_raft_shard_group(psh))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    }
    if (rc == EFS_OK) {
        rc = efs_meta_apply_lookup(h->kv, new_parent, new_name, &dent);
        if (rc == EFS_OK) {
            pthread_mutex_unlock(&h->read_mu);
            set_inode_rc(out, EFS_ERR_EXIST, hint);
            return;
        }
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
    }
    if (rc == EFS_OK) {
        uint64_t bit;

        memset(&ndent, 0, sizeof(ndent));
        ndent.ino = row.ino;
        ndent.generation = row.generation;
        ndent.type = row.mode & S_IFMT;
        row.nlink++;
        if (row.base_ctime < now)
            row.base_ctime = now;
        d_lane = dprow.layout == EFS_META_LAYOUT_LOCAL
                     ? 0
                     : efs_kv_dir_lane(new_name);
        if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
            if (dprow.base_mtime < now)
                dprow.base_mtime = now;
            if (dprow.base_ctime < now)
                dprow.base_ctime = now;
            efs_meta_dir_note_entry(&dprow, 1);
            touch_parent = 1;
        } else {
            bit = 1ull << d_lane;
            if ((dprow.used_shards & bit) == 0) {
                dprow.used_shards |= bit;
                touch_parent = 1;
            }
            stamp_lane = 1;
        }
        rc = efs_meta_pack_dentry(&ndent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
        if (rc == EFS_OK && touch_parent)
            rc = efs_meta_pack_inode(&dprow, v_par, sizeof(v_par));
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(dsh, new_parent, new_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, src_ino, k_ino, &ki);
    if (rc == EFS_OK && touch_parent)
        rc = efs_kv_key_inode(psh, new_parent, k_par, &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, new_parent, d_lane, k_dseq, &ks);
    if (rc == EFS_OK && stamp_lane)
        rc = efs_meta_stamp_dir_lane(h->kv, &dprow, new_name, now, k_ln, &kln,
                                     v_ln, sizeof(v_ln));
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_ino, ki, &iver);
    if (rc == EFS_OK && touch_parent)
        rc = efs_txn_ver_get(h->kv, k_par, kp, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc == EFS_OK && stamp_lane)
        rc = efs_txn_ver_get(h->kv, k_ln, kln, &lnver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        rc = host_parts_add(&parts, dsh);
        if (rc == EFS_OK)
            rc = host_parts_add(&parts, ish);
        if (rc == EFS_OK && touch_parent)
            rc = host_parts_add(&parts, psh);
        if (rc != EFS_OK)
            goto link_prepped;
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == dsh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dent, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dseq, ks,
                                   sver, EFS_TXN_PUT, v_dseq, 8, &hint);
                if (rc == EFS_OK && stamp_lane)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_ln, kln,
                                   lnver, EFS_TXN_PUT, v_ln, sizeof(v_ln),
                                   &hint);
            }
            if (rc == EFS_OK && touch_parent && sh == psh)
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_par, kp,
                               pver, EFS_TXN_PUT, v_par, sizeof(v_par), &hint);
            if (rc == EFS_OK && sh == ish) {
                rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                               iver, EFS_TXN_PUT, v_ino, sizeof(v_ino),
                               &hint);
            }
        }
link_prepped:
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22,
                                   &hint);
            for (i = 0; i < parts.n && rc == EFS_OK; i++) {
                pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
                rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]),
                                       cmd, 22, &hint);
            }
        }
    }
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, src_ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, src_ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = new_parent;
        strncpy(out->inode.name, new_name, EFS_MAX_NAME - 1);
    }
}

/* RENAME: src dentry drop + dest dentry PUT + inode parent/ctime as a
 * txn (same PREPARE/DECIDE/RESOLVE as LINK). LOCAL stamps the parent
 * row; HASHED/SPLITTING stamp dir-lanes (parent row only for
 * used_shards / nlink). SPLITTING dest writes hashed; src drop is I8
 * (HASHED=TOMBSTONE + DEL local leftover). Leftover dest is POSIX
 * replace: PUT hashed dest + DEL the local leftover. Same-dir is one
 * parent; cross-dir stamps both. A directory GUARDs dst_parent
 * ancestry pver sidecars and exclusive-PUTs its own pver (cycle
 * prevention). A dest dir that is itself SPLITTING stays BUSY.
 * Replacing an existing dest file is POSIX replace (nlink-- / DEL).
 * Replacing an empty dest dir is dest rmdir (LOCAL or HASHED; HASHED
 * GUARDs used-lane dseqs, cap EFS_TXN_MAX_PART → BUSY). HASHED dentries
 * and scattered dir inodes bounce if this replica does not host every
 * participant. */
void server_raft_host_rename_at(efs_ino_t old_parent, const char *old_name,
                                efs_ino_t new_parent, const char *new_name,
                                struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, dprow, row, nrow;
    struct efs_meta_dentry dent, ndent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct host_pver_guard gv[EFS_TXN_MAX_PART];
    uint8_t k_dst[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_par[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX], k_pver[EFS_KV_KEY_MAX];
    uint8_t k_nino[EFS_KV_KEY_MAX], k_ndseq[EFS_KV_KEY_MAX];
    uint8_t k_dpar[EFS_KV_KEY_MAX], k_ddseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_ino[EFS_META_INO_BYTES];
    uint8_t v_par[EFS_META_INO_BYTES], v_dseq[8], v_pver[8], sb[8], cmd[22];
    uint8_t v_nino[EFS_META_INO_BYTES], v_dpar[EFS_META_INO_BYTES], v_ddseq[8];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint8_t k_sln[EFS_KV_KEY_MAX], v_sln[EFS_META_LANE_BYTES];
    uint8_t k_dln[EFS_KV_KEY_MAX], v_dln[EFS_META_LANE_BYTES];
    uint32_t kd = 0, ki = 0, kp = 0, kq = 0, kpv = 0, sn = 8;
    uint32_t kn = 0, knd = 0, krl = 0, kdp = 0, kdsq = 0, ksln = 0, kdln = 0;
    uint32_t ssh, dsh, ish, psh, dpsh, coord, nsh = 0, ash = 0;
    uint64_t dver = 0, iver = 0, pver = 0, qver = 0, ever = 0;
    uint64_t nver = 0, gver2 = 0, rver = 0, dpver = 0, dsver = 0;
    uint64_t slver = 0, dlver = 0;
    uint64_t seq = 0, dseqn = 0, now;
    int hint = -1;
    int rc, i, gr, is_dir = 0, ngv = 0, nxd = 0, hashed_xdir = 0;
    int xist = 0, xdir = 0, xput = 0, same = 0;
    int touch_src = 0, stamp_src = 0, touch_dst = 0, stamp_dst = 0, two_dseq = 0;
    uint8_t s_lane = 0, d_lane = 0;
    struct host_pver_guard xdseq[EFS_TXN_MAX_PART];
    struct host_dent_drop drop;
    uint8_t k_dloc[EFS_KV_KEY_MAX];
    uint32_t kdl = 0;
    uint64_t dloc_ver = 0;
    int dest_del_loc = 0;

    memset(out, 0, sizeof(*out));
    memset(&drop, 0, sizeof(drop));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !old_name || !new_name || old_parent == 0 ||
        new_parent == 0 || old_name[0] == '\0' || new_name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (old_parent == new_parent && strcmp(old_name, new_name) == 0) {
        out->status = EFS_INODE_RPC_OK;
        return;
    }
    now = now_ns();
    same = old_parent == new_parent;
    psh = efs_kv_inode_shard(old_parent);
    dpsh = efs_kv_inode_shard(new_parent);
    {
        uint8_t need[2];
        int nn = 1;
        need[0] = efs_raft_shard_group(psh);
        if (efs_raft_shard_group(dpsh) != need[0])
            need[nn++] = efs_raft_shard_group(dpsh);
        if (!host_hosts_all(h, need, nn)) {
            host_fwd_rename(h, old_parent, old_name, new_parent, new_name, out,
                            need, nn);
            return;
        }
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, old_parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL &&
        prow.layout != EFS_META_LAYOUT_HASHED &&
        prow.layout != EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && !same) {
        if (efs_raft_shard_group(dpsh) != efs_raft_shard_group(psh))
            rc = host_read_index(h, efs_raft_shard_group(dpsh), &hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_get_inode(h->kv, new_parent, &dprow);
        if (rc == EFS_OK && !S_ISDIR(dprow.mode))
            rc = EFS_ERR_INVAL;
        if (rc == EFS_OK && dprow.layout != EFS_META_LAYOUT_LOCAL &&
            dprow.layout != EFS_META_LAYOUT_HASHED &&
            dprow.layout != EFS_META_LAYOUT_SPLITTING)
            rc = EFS_ERR_INVAL;
    }
    ssh = (rc == EFS_OK) ? efs_kv_dentry_shard(old_parent, old_name, prow.layout)
                         : 0;
    dsh = (rc == EFS_OK)
              ? efs_kv_dentry_shard(new_parent, new_name,
                                   same ? prow.layout : dprow.layout)
              : 0;
    /* HASHED dentries live on a dir-lane shard that may be another Raft
     * group. Bounce before lookup: a local KV miss would look like ENOENT. */
    if (rc == EFS_OK) {
        uint8_t gs[8];
        int ngs = 0;
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(psh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(dpsh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(ssh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(dsh));
        if (!host_hosts_all(h, gs, ngs)) {
            uint8_t need[2];
            need[0] = EFS_RAFT_GROUP_SHARD;
            need[1] = EFS_RAFT_GROUP_SHARD2;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_rename(h, old_parent, old_name, new_parent, new_name, out,
                            need, 2);
            return;
        }
    }
    if (rc == EFS_OK && efs_raft_shard_group(ssh) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(ssh), &hint);
    if (rc == EFS_OK && efs_raft_shard_group(dsh) != efs_raft_shard_group(psh) &&
        efs_raft_shard_group(dsh) != efs_raft_shard_group(ssh) &&
        efs_raft_shard_group(dsh) != efs_raft_shard_group(dpsh))
        rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, old_parent, old_name, &dent);
    if (rc == EFS_OK && (dent.type & S_IFMT) == S_IFDIR)
        is_dir = 1;
    if (rc == EFS_OK && is_dir && dent.ino == EFS_ROOT_INO)
        rc = EFS_ERR_INVAL;
    /* Rename-over-existing (POSIX): the PUT at k_dst overwrites the dest
     * dentry in place; the dest INODE ROW is handled below (nlink-- / DEL,
     * or the empty-dir checks for a dir dest). A dest dentry naming the
     * same (ino,gen) as the source is a hardlink self-rename: no dest
     * handling, the txn just moves the name (old-monolith behaviour). */
    if (rc == EFS_OK) {
        int xrc = efs_meta_apply_lookup(h->kv, new_parent, new_name, &ndent);
        if (xrc == EFS_OK) {
            if (ndent.ino == dent.ino && ndent.generation == dent.generation) {
                /* same inode: no dest row to retire */
            } else {
                xist = 1;
                xdir = (ndent.type & S_IFMT) == S_IFDIR;
                nsh = efs_kv_inode_shard(ndent.ino);
                if (is_dir && !xdir)
                    rc = EFS_ERR_INVAL; /* dir over non-dir: ENOTDIR */
                else if (!is_dir && xdir)
                    rc = EFS_ERR_INVAL; /* non-dir over dir: EISDIR */
            }
        } else if (xrc != EFS_ERR_NOT_FOUND) {
            rc = xrc;
        }
    }
    /* Bounce before resolve: the child inode row can live on another group
     * (a scattered dir inode, or a file hardlinked into a dir on a
     * different group), and resolve maps a missing row to I9 (EFS_ERR_IO).
     * Dest parent and an existing dest's inode row are the same. */
    if (rc == EFS_OK) {
        uint32_t csh = efs_kv_inode_shard(dent.ino);
        uint8_t gs[8];
        int ngs = 0, j, all;
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(psh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(dpsh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(ssh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(dsh));
        bounce_add(gs, &ngs, 8, efs_raft_shard_group(csh));
        if (xist)
            bounce_add(gs, &ngs, 8, efs_raft_shard_group(nsh));
        all = 1;
        for (j = 0; j < ngs; j++)
            if (!host_hosts(h, gs[j]))
                all = 0;
        if (!all) {
            uint8_t need[2];
            need[0] = EFS_RAFT_GROUP_SHARD;
            need[1] = EFS_RAFT_GROUP_SHARD2;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_rename(h, old_parent, old_name, new_parent, new_name, out,
                            need, 2);
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_resolve(h->kv, old_parent, old_name, &dent, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        is_dir = 1;
    if (rc == EFS_OK && is_dir && row.ino == EFS_ROOT_INO)
        rc = EFS_ERR_INVAL;
    ish = (rc == EFS_OK) ? efs_kv_inode_shard(row.ino) : 0;
    if (rc == EFS_OK && efs_raft_shard_group(ish) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(ish), &hint);
    /* Existing dest's inode row. Dir dest: rmdir rules (LOCAL empty by
     * nlink + readdir, HASHED empty by readdir + used-lane dseq GUARDs,
     * no open leases; parent loses a subdir). File dest: unlink rules
     * (nlink--, or DEL on last link; a leased last link keeps an nlink=0
     * row for open fds). */
    if (rc == EFS_OK && xist)
        rc = efs_meta_apply_get_inode(h->kv, ndent.ino, &nrow);
    if (rc == EFS_OK && xist && efs_raft_shard_group(nsh) !=
        efs_raft_shard_group(psh) && efs_raft_shard_group(nsh) !=
        efs_raft_shard_group(ish))
        rc = host_read_index(h, efs_raft_shard_group(nsh), &hint);
    if (rc == EFS_OK && xist && xdir) {
        struct efs_meta_dir_cursor cur;
        struct efs_meta_dir_ent one;
        uint32_t nent = 0;
        int held;
        if (nrow.layout == EFS_META_LAYOUT_SPLITTING)
            rc = EFS_ERR_BUSY;
        else if (nrow.layout != EFS_META_LAYOUT_LOCAL &&
                 nrow.layout != EFS_META_LAYOUT_HASHED)
            rc = EFS_ERR_INVAL;
        else if (nrow.nlink > 2)
            rc = EFS_ERR_NOT_EMPTY;
        if (rc == EFS_OK && nrow.layout == EFS_META_LAYOUT_HASHED) {
            uint8_t gs[8];
            int ngs = 0, lane;

            hashed_xdir = 1;
            bounce_add(gs, &ngs, 8, efs_raft_shard_group(psh));
            bounce_add(gs, &ngs, 8, efs_raft_shard_group(dpsh));
            bounce_add(gs, &ngs, 8, efs_raft_shard_group(ssh));
            bounce_add(gs, &ngs, 8, efs_raft_shard_group(dsh));
            bounce_add(gs, &ngs, 8, efs_raft_shard_group(ish));
            bounce_add(gs, &ngs, 8, efs_raft_shard_group(nsh));
            for (lane = 0; lane < EFS_META_LANES; lane++) {
                if ((nrow.used_shards & (1ull << lane)) == 0)
                    continue;
                bounce_add(gs, &ngs, 8,
                           efs_raft_shard_group(efs_kv_lane_shard(nrow.ino,
                                                                (uint8_t)lane)));
            }
            if (!host_hosts_all(h, gs, ngs)) {
                uint8_t need[2];

                host_need_both(need);
                pthread_mutex_unlock(&h->read_mu);
                host_fwd_rename(h, old_parent, old_name, new_parent, new_name,
                                out, need, 2);
                return;
            }
            rc = host_read_inode_lanes(h, nrow.ino, &hint);
        }
        if (rc == EFS_OK) {
            memset(&cur, 0, sizeof(cur));
            memset(&one, 0, sizeof(one));
            rc = efs_meta_apply_readdir(h->kv, nrow.ino, &cur, &one, 1, &nent);
            if (rc == EFS_OK && nent > 0)
                rc = EFS_ERR_NOT_EMPTY;
        }
        if (rc == EFS_OK) {
            held = efs_lease_any(h->kv, nrow.ino, nrow.generation);
            if (held < 0)
                rc = held;
            else if (held)
                rc = EFS_ERR_BUSY;
        }
        if (rc == EFS_OK && hashed_xdir) {
            uint8_t lane;

            for (lane = 0; lane < EFS_META_LANES && rc == EFS_OK; lane++) {
                uint32_t lsh;

                if ((nrow.used_shards & (1ull << lane)) == 0)
                    continue;
                if (nxd >= EFS_TXN_MAX_PART) {
                    rc = EFS_ERR_BUSY;
                    break;
                }
                lsh = efs_kv_lane_shard(nrow.ino, (uint8_t)lane);
                xdseq[nxd].shard = lsh;
                xdseq[nxd].klen = 0;
                rc = efs_kv_key_dseq(lsh, nrow.ino, (uint8_t)lane, xdseq[nxd].key,
                                      &xdseq[nxd].klen);
                if (rc == EFS_OK)
                    rc = efs_txn_ver_get(h->kv, xdseq[nxd].key, xdseq[nxd].klen,
                                         &xdseq[nxd].ver);
                if (rc == EFS_OK)
                    nxd++;
            }
        }
        if (rc == EFS_OK && (same ? prow.nlink : dprow.nlink) < 3)
            rc = EFS_ERR_PROTO;
        if (rc == EFS_OK) {
            if (same)
                prow.nlink--;
            else
                dprow.nlink--;
        }
    }
    if (rc == EFS_OK && xist && !xdir) {
        if (nrow.nlink <= 1) {
            int held = efs_lease_any(h->kv, nrow.ino, nrow.generation);
            if (held < 0)
                rc = held;
            else if (held) {
                nrow.nlink = 0;
                if (nrow.base_ctime < now)
                    nrow.base_ctime = now;
                xput = 1;
            }
        } else {
            nrow.nlink--;
            if (nrow.base_ctime < now)
                nrow.base_ctime = now;
            xput = 1;
        }
    }
    if (rc == EFS_OK && xist && xput)
        rc = efs_meta_pack_inode(&nrow, v_nino, sizeof(v_nino));
    /* A file dest retired at its last link (the DEL below) needs its reap
     * marker (L7) in the same txn, on the dest inode group's anchor
     * shard. */
    if (rc == EFS_OK && xist && !xdir && !xput) {
        ash = efs_kv_anchor_shard(nsh);
        rc = efs_kv_key_reap(ash, nrow.ino, k_reap, &krl);
        if (rc == EFS_OK)
            efs_meta_pack_reap(v_reap, nrow.generation, nrow.active_lanes);
    }
    if (rc == EFS_OK && is_dir && !same) {
        if (prow.nlink < 3)
            rc = EFS_ERR_PROTO;
        else {
            prow.nlink--;
            dprow.nlink++;
        }
    }
    if (rc == EFS_OK) {
        uint64_t bit;
        s_lane = prow.layout == EFS_META_LAYOUT_LOCAL
                     ? 0
                     : efs_kv_dir_lane(old_name);
        d_lane = (same ? prow.layout : dprow.layout) == EFS_META_LAYOUT_LOCAL
                     ? 0
                     : efs_kv_dir_lane(new_name);
        if (prow.layout == EFS_META_LAYOUT_LOCAL) {
            if (prow.base_mtime < now)
                prow.base_mtime = now;
            if (prow.base_ctime < now)
                prow.base_ctime = now;
            if (!(same && !xist))
                efs_meta_dir_note_entry(&prow, -1);
            touch_src = 1;
        } else {
            bit = 1ull << s_lane;
            if ((prow.used_shards & bit) == 0) {
                prow.used_shards |= bit;
                touch_src = 1;
            }
            stamp_src = 1;
        }
        if (!same) {
            if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
                if (dprow.base_mtime < now)
                    dprow.base_mtime = now;
                if (dprow.base_ctime < now)
                    dprow.base_ctime = now;
                if (!xist)
                    efs_meta_dir_note_entry(&dprow, 1);
                touch_dst = 1;
            } else {
                bit = 1ull << d_lane;
                if ((dprow.used_shards & bit) == 0) {
                    dprow.used_shards |= bit;
                    touch_dst = 1;
                }
                stamp_dst = 1;
            }
        } else if (prow.layout != EFS_META_LAYOUT_LOCAL) {
            bit = 1ull << d_lane;
            if ((prow.used_shards & bit) == 0) {
                prow.used_shards |= bit;
                touch_src = 1;
            }
            stamp_dst = 1;
        }
        if (is_dir && !same) {
            touch_src = 1;
            touch_dst = 1;
        }
        if (xist && xdir) {
            if (same)
                touch_src = 1;
            else
                touch_dst = 1;
        }
        if (stamp_src && stamp_dst && ssh == dsh && s_lane == d_lane)
            stamp_dst = 0;
        two_dseq = !(ssh == dsh && s_lane == d_lane);
    }
    memset(&parts, 0, sizeof(parts));
    if (rc == EFS_OK)
        rc = host_parts_add(&parts, ssh);
    if (rc == EFS_OK && dsh != ssh)
        rc = host_parts_add(&parts, dsh);
    if (rc == EFS_OK)
        rc = host_parts_add(&parts, ish);
    if (rc == EFS_OK && touch_src)
        rc = host_parts_add(&parts, psh);
    if (rc == EFS_OK && touch_dst)
        rc = host_parts_add(&parts, dpsh);
    if (rc == EFS_OK && xist)
        rc = host_parts_add(&parts, nsh);
    if (rc == EFS_OK && xist && !xdir && !xput)
        rc = host_parts_add(&parts, ash);
    for (i = 0; i < nxd && rc == EFS_OK; i++)
        rc = host_parts_add(&parts, xdseq[i].shard);
    if (rc == EFS_OK && is_dir) {
        rc = host_pver_guard_chain(h, new_parent, row.ino, &parts, gv, &ngv,
                                   &hint);
        if (rc == EFS_OK)
            row.parent_version++;
    }
    if (rc == EFS_OK) {
        memset(&ndent, 0, sizeof(ndent));
        ndent.ino = row.ino;
        ndent.generation = row.generation;
        ndent.type = row.mode & S_IFMT;
        row.parent = new_parent;
        if (row.base_ctime < now)
            row.base_ctime = now;
        rc = efs_meta_pack_dentry(&ndent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
        if (rc == EFS_OK && touch_src)
            rc = efs_meta_pack_inode(&prow, v_par, sizeof(v_par));
        if (rc == EFS_OK && touch_dst)
            rc = efs_meta_pack_inode(&dprow, v_dpar, sizeof(v_dpar));
    }
    if (rc == EFS_OK)
        rc = host_dent_drop_fill(h->kv, &prow, old_parent, old_name, &drop);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(dsh, new_parent, new_name, k_dst, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, row.ino, k_ino, &ki);
    if (rc == EFS_OK && touch_src)
        rc = efs_kv_key_inode(psh, old_parent, k_par, &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(ssh, old_parent, s_lane, k_dseq, &kq);
    if (rc == EFS_OK && touch_dst)
        rc = efs_kv_key_inode(dpsh, new_parent, k_dpar, &kdp);
    if (rc == EFS_OK && two_dseq)
        rc = efs_kv_key_dseq(dsh, new_parent, d_lane, k_ddseq, &kdsq);
    if (rc == EFS_OK && stamp_src)
        rc = efs_meta_stamp_dir_lane(h->kv, &prow, old_name, now, k_sln, &ksln,
                                     v_sln, sizeof(v_sln));
    if (rc == EFS_OK && stamp_dst)
        rc = efs_meta_stamp_dir_lane(h->kv, same ? &prow : &dprow, new_name, now,
                                     k_dln, &kdln, v_dln, sizeof(v_dln));
    if (rc == EFS_OK && xist)
        rc = efs_kv_key_inode(nsh, nrow.ino, k_nino, &kn);
    if (rc == EFS_OK && xist && xdir && !hashed_xdir)
        rc = efs_kv_key_dseq(nsh, nrow.ino, 0, k_ndseq, &knd);
    if (rc == EFS_OK && is_dir) {
        rc = efs_kv_key_pver(ish, row.ino, k_pver, &kpv);
        if (rc == EFS_OK)
            rc = efs_txn_ver_get(h->kv, k_pver, kpv, &ever);
        if (rc == EFS_OK)
            wr64be(v_pver, row.parent_version);
    }
    if (rc == EFS_OK && xist) {
        uint8_t dl = same ? prow.layout : dprow.layout;
        if (dl == EFS_META_LAYOUT_SPLITTING) {
            rc = efs_kv_key_dentry(dpsh, new_parent, new_name, k_dloc, &kdl);
            if (rc == EFS_OK &&
                !(kdl == kd && memcmp(k_dloc, k_dst, kdl) == 0)) {
                dest_del_loc = 1;
                rc = efs_txn_ver_get(h->kv, k_dloc, kdl, &dloc_ver);
            }
        }
    }
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dst, kd, &dver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_ino, ki, &iver);
    if (rc == EFS_OK && touch_src)
        rc = efs_txn_ver_get(h->kv, k_par, kp, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, kq, &qver);
    if (rc == EFS_OK && touch_dst)
        rc = efs_txn_ver_get(h->kv, k_dpar, kdp, &dpver);
    if (rc == EFS_OK && two_dseq)
        rc = efs_txn_ver_get(h->kv, k_ddseq, kdsq, &dsver);
    if (rc == EFS_OK && stamp_src)
        rc = efs_txn_ver_get(h->kv, k_sln, ksln, &slver);
    if (rc == EFS_OK && stamp_dst)
        rc = efs_txn_ver_get(h->kv, k_dln, kdln, &dlver);
    if (rc == EFS_OK && xist)
        rc = efs_txn_ver_get(h->kv, k_nino, kn, &nver);
    if (rc == EFS_OK && xist && xdir && !hashed_xdir)
        rc = efs_txn_ver_get(h->kv, k_ndseq, knd, &gver2);
    if (rc == EFS_OK && xist && !xdir && !xput)
        rc = efs_txn_ver_get(h->kv, k_reap, krl, &rver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, kq, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        if (two_dseq) {
            sn = 8;
            gr = efs_kv_get(h->kv, k_ddseq, kdsq, sb, &sn);
            dseqn = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
            wr64be(v_ddseq, dseqn + 1);
        }
        if (rc == EFS_OK && drop.del_loc)
            rc = host_parts_add(&parts, drop.psh);
        if (rc == EFS_OK && (drop.put_tomb || drop.del_hash))
            rc = host_parts_add(&parts, drop.hsh);
        if (rc == EFS_OK && dest_del_loc)
            rc = host_parts_add(&parts, dpsh);
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            rc = host_dent_drop_prep(h, sh, &drop, &t, &parts, &hint);
            if (rc == EFS_OK && dest_del_loc && sh == dpsh)
                rc = host_prep(h, dpsh, EFS_TXN_EXCL, &t, &parts, k_dloc, kdl,
                               dloc_ver, EFS_TXN_DEL, NULL, 0, &hint);
            if (rc == EFS_OK && sh == ssh) {
                if (dsh == ssh)
                    rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_dst, kd,
                                   dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                                   &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_dseq, kq,
                                   qver, EFS_TXN_PUT, v_dseq, 8, &hint);
                if (rc == EFS_OK && stamp_src)
                    rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_sln, ksln,
                                   slver, EFS_TXN_PUT, v_sln, sizeof(v_sln),
                                   &hint);
                if (rc == EFS_OK && stamp_dst && dsh == ssh)
                    rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_dln, kdln,
                                   dlver, EFS_TXN_PUT, v_dln, sizeof(v_dln),
                                   &hint);
                if (rc == EFS_OK && two_dseq && dsh == ssh)
                    rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_ddseq,
                                   kdsq, dsver, EFS_TXN_PUT, v_ddseq, 8, &hint);
            }
            if (rc == EFS_OK && sh == dsh && dsh != ssh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dst, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               &hint);
                if (rc == EFS_OK && two_dseq)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_ddseq,
                                   kdsq, dsver, EFS_TXN_PUT, v_ddseq, 8,
                                   &hint);
                if (rc == EFS_OK && stamp_dst)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dln, kdln,
                                   dlver, EFS_TXN_PUT, v_dln, sizeof(v_dln),
                                   &hint);
            }
            if (rc == EFS_OK && touch_src && sh == psh)
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_par, kp,
                               pver, EFS_TXN_PUT, v_par, sizeof(v_par),
                               &hint);
            if (rc == EFS_OK && touch_dst && sh == dpsh)
                rc = host_prep(h, dpsh, EFS_TXN_EXCL, &t, &parts, k_dpar, kdp,
                               dpver, EFS_TXN_PUT, v_dpar, sizeof(v_dpar),
                               &hint);
            if (rc == EFS_OK && sh == ish) {
                rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                               iver, EFS_TXN_PUT, v_ino, sizeof(v_ino),
                               &hint);
                if (rc == EFS_OK && is_dir)
                    rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_pver,
                                   kpv, ever, EFS_TXN_PUT, v_pver, 8, &hint);
            }
            if (rc == EFS_OK && xist && sh == nsh) {
                if (xput)
                    rc = host_prep(h, nsh, EFS_TXN_EXCL, &t, &parts, k_nino,
                                   kn, nver, EFS_TXN_PUT, v_nino,
                                   sizeof(v_nino), &hint);
                else
                    rc = host_prep(h, nsh, EFS_TXN_EXCL, &t, &parts, k_nino,
                                   kn, nver, EFS_TXN_DEL, NULL, 0, &hint);
                if (rc == EFS_OK && xdir && !hashed_xdir)
                    rc = host_prep(h, nsh, EFS_TXN_GUARD, &t, &parts, k_ndseq,
                                   knd, gver2, 0, NULL, 0, &hint);
            }
            if (rc == EFS_OK && xist && !xdir && !xput && sh == ash &&
                ash != ssh && ash != dsh && ash != ish && ash != nsh &&
                ash != psh && ash != dpsh)
                rc = host_prep(h, ash, EFS_TXN_EXCL, &t, &parts, k_reap, krl,
                               rver, EFS_TXN_PUT, v_reap, sizeof(v_reap),
                               &hint);
        }
        if (rc == EFS_OK && xist && !xdir && !xput &&
            (ash == ssh || ash == dsh || ash == ish || ash == nsh ||
             ash == psh || ash == dpsh))
            /* The anchor shard aliases another participant; the loop's
             * dedicated branch skipped it, so the marker PREP goes to the
             * anchor's own group here. */
            rc = host_prep(h, ash, EFS_TXN_EXCL, &t, &parts, k_reap, krl,
                           rver, EFS_TXN_PUT, v_reap, sizeof(v_reap),
                           &hint);
        for (i = 0; i < ngv && rc == EFS_OK; i++)
            rc = host_prep(h, gv[i].shard, EFS_TXN_GUARD, &t, &parts, gv[i].key,
                           gv[i].klen, gv[i].ver, 0, NULL, 0, &hint);
        for (i = 0; i < nxd && rc == EFS_OK; i++)
            rc = host_prep(h, xdseq[i].shard, EFS_TXN_GUARD, &t, &parts,
                           xdseq[i].key, xdseq[i].klen, xdseq[i].ver, 0, NULL, 0,
                           &hint);
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22,
                                   &hint);
            for (i = 0; i < parts.n && rc == EFS_OK; i++) {
                pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
                rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]),
                                       cmd, 22, &hint);
            }
        }
    }
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, row.ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, row.ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = new_parent;
        strncpy(out->inode.name, new_name, EFS_MAX_NAME - 1);
    }
}

/* OPEN reservations whose range is at or below the published size become
 * COMPLETED. No new opcode: REPORT already committed the data. Catch-up
 * with nopen==0 is a no-op (existing raft-smoke-p). read_mu held. */
static int host_resolve_caught_up(struct efs_raft_host *h, efs_ino_t ino,
                                  uint64_t sz, int *hint)
{
    uint64_t offs[64], lens[64];
    uint32_t n = 64, i, clen = 0;
    uint8_t cmd[HOST_APPEND_RES_LEN];
    uint8_t ig;
    int rc;

    rc = efs_meta_apply_append_open(h->kv, ino, offs, lens, &n);
    /* Deleted inode (stale report skipped by host_pub_locked): no
     * reservations to resolve. */
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    for (i = 0; i < n; i++) {
        if (offs[i] + lens[i] > sz)
            continue;
        rc = pack_append_res_cmd(cmd, &clen, ino, offs[i],
                                 EFS_META_APPEND_COMPLETED);
        if (rc != EFS_OK)
            return rc;
        rc = host_propose_wait(h, ig, cmd, clen, hint);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

/* Files and symlinks carry a chunk map; FUSE stores a symlink target as
 * ordinary published bytes. Directories do not. */
static int host_holds_chunks(uint32_t mode)
{
    return S_ISREG(mode) || S_ISLNK(mode);
}

/* One chunk CAS + lane MAX. read_mu held. First-use of a lane whose
 * group is not the inode's is INVAL this slice (that is a 2-shard txn).
 * Lane 0 is the inode shard, so the smoke's first chunk is one group. */
/* The wire chunk rec carries no client op identity, so the publish
 * candidate is content-addressed: re-reporting the same fragments mints
 * the same candidate (a true replay no-op at the CAS), while new content
 * mints a new identity (the CAS proceeds). A constant candidate makes
 * every overwrite of an already-published chunk a false "replay" and
 * keeps the stale checksums forever — the reader then short-circuits on
 * the old zero-hash and serves zeros over live data. */
static uint64_t host_pub_candidate_gen(const struct efs_chunk_rec *rec,
                                       uint32_t chunk_index)
{
    uint8_t h[EFS_HASH_SIZE];
    uint8_t in[sizeof(rec->nodes) + sizeof(rec->checksums)];

    memcpy(in, rec->nodes, sizeof(rec->nodes));
    memcpy(in + sizeof(rec->nodes), rec->checksums, sizeof(rec->checksums));
    efs_hash(in, sizeof(in), h);
    return efs_meta_candidate_gen(h, 0, 1, chunk_index, 0);
}

static int host_pub_locked(struct efs_raft_host *h, const struct efs_chunk_rec *rec,
                           uint64_t new_size, int *hint)
{
    struct efs_meta_pub p;
    struct efs_meta_row row;
    struct efs_meta_chunk got;
    uint8_t cmd[HOST_PUBLISH_LEN];
    uint32_t clen = 0, lsh, ish;
    uint64_t idx = 0;
    uint8_t lane, lg, ig;
    int rc, i;

    if (!rec || rec->ino == 0)
        return EFS_ERR_INVAL;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (rec->nodes[i] == 0) {
            if (env_on("EFS_RAFT_DBG"))
                fprintf(stderr, "raft-host: pub ino=%llu ci=%u INVAL node0\n",
                        (unsigned long long)rec->ino, rec->chunk_index);
            return EFS_ERR_INVAL;
        }
    }
    ish = efs_kv_inode_shard(rec->ino);
    ig = efs_raft_shard_group(ish);
    rc = host_read_index(h, ig, hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, rec->ino, &row);
    /* Stale publish for a deleted inode: skip the proposal entirely (P3).
     * ReadIndex guarantees we see the committed create, so NOT_FOUND here
     * means the unlink already won. */
    if (rc == EFS_ERR_NOT_FOUND) {
        if (env_on("EFS_RAFT_DBG"))
            fprintf(stderr, "raft-host: pub ino=%llu ci=%u stale (deleted), skip\n",
                    (unsigned long long)rec->ino, rec->chunk_index);
        return EFS_OK;
    }
    if (rc != EFS_OK) {
        if (env_on("EFS_RAFT_DBG"))
            fprintf(stderr, "raft-host: pub ino=%llu ci=%u get_inode rc=%d\n",
                    (unsigned long long)rec->ino, rec->chunk_index, rc);
        return rc;
    }
    if (!host_holds_chunks(row.mode)) {
        if (env_on("EFS_RAFT_DBG"))
            fprintf(stderr, "raft-host: pub ino=%llu ci=%u INVAL mode=%o\n",
                    (unsigned long long)rec->ino, rec->chunk_index, row.mode);
        return EFS_ERR_INVAL;
    }
    lane = (uint8_t)(rec->chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(rec->ino, lane);
    lg = efs_raft_shard_group(lsh);
    if (lg != ig && (row.active_lanes & (1ULL << lane)) == 0) {
        /* First use of a cross-group lane: register it in the inode row's
         * active_lanes bitmap on the INODE group first (that bitmap is
         * stat()'s collect set), then publish lane-local below. A crash
         * between the two leaves the bit set with no chunk — harmless
         * (stat treats an absent lane as 0). */
        uint8_t acmd[HOST_ACTIVATE_LANE_LEN];
        uint32_t alen = 0;

        rc = pack_activate_lane_cmd(acmd, &alen, rec->ino, lane);
        if (rc == EFS_OK)
            rc = host_propose_wait(h, ig, acmd, alen, hint);
        if (rc != EFS_OK)
            return rc;
    }
    if (lg != ig)
        rc = host_read_index(h, lg, hint);
    if (rc != EFS_OK)
        return rc;
    memset(&got, 0, sizeof(got));
    rc = efs_meta_apply_get_chunk(h->kv, rec->ino, rec->chunk_index, &got);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    else if (rc != EFS_OK)
        return rc;
    memset(&p, 0, sizeof(p));
    p.ino = rec->ino;
    p.chunk_index = rec->chunk_index;
    p.new_size = new_size;
    p.now = now_ns();
    p.expected_gen = got.generation;
    p.candidate_gen = host_pub_candidate_gen(rec, rec->chunk_index);
    if (p.candidate_gen == 0)
        p.candidate_gen = 1;
    p.content_epoch = row.content_epoch;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    memcpy(p.ch.nodes, rec->nodes, sizeof(p.ch.nodes));
    memcpy(p.ch.checksums, rec->checksums, sizeof(p.ch.checksums));
    if (lg != ig) {
        /* Lane-local: the lane group's KV has no inode row. The FileID
         * fields ride the entry (read above under ReadIndex on the inode
         * group); the lane's own fenced_epoch is the truncate fence. */
        p.inode_gen = row.generation;
        p.mtime_gen = row.mtime_gen;
        p.lane_local = 1;
    }
    rc = pack_publish_cmd(cmd, &clen, &p);
    if (rc == EFS_OK)
        rc = host_propose(h, lg, cmd, clen, &idx, hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, lg, idx, hint);
    if (rc != EFS_OK && env_on("EFS_RAFT_DBG"))
        fprintf(stderr, "raft-host: pub ino=%llu ci=%u propose/apply rc=%d\n",
                (unsigned long long)rec->ino, rec->chunk_index, rc);
    return rc;
}

void server_raft_host_report(const struct efs_chunk_rec *recs, uint32_t count,
                             const struct efs_ino_size_rec *irecs,
                             uint32_t ino_count, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    uint32_t i, j;
    uint64_t sz;
    int hint = -1;
    int rc = EFS_OK;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (count == 0) {
        out->status = EFS_INODE_RPC_OK;
        return;
    }
    if (!recs) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (env_on("EFS_RAFT_DBG"))
        fprintf(stderr, "raft-host: report count=%u ino_count=%u\n",
                count, ino_count);
    /* The batch can touch both groups (per-rec inode group + lane group).
     * Every group it needs must be local — a publish proposed to a group
     * this node doesn't vote in could not be waited on, and its apply on
     * a voter lacking the inode row would diverge. Bounce the whole batch
     * to a dual-host (never self, so no forward loop). */
    {
        uint8_t need[HOST_NGROUPS];
        int nn = 0;

        for (i = 0; i < count; i++) {
            uint8_t gs[2];
            int k, f;

            gs[0] = efs_raft_shard_group(efs_kv_inode_shard(recs[i].ino));
            gs[1] = efs_raft_shard_group(
                efs_kv_lane_shard(recs[i].ino,
                                  (uint8_t)(recs[i].chunk_index %
                                            EFS_META_LANES)));
            for (k = 0; k < 2; k++) {
                for (f = 0; f < nn; f++)
                    if (need[f] == gs[k])
                        break;
                if (f == nn && nn < HOST_NGROUPS)
                    need[nn++] = gs[k];
            }
        }
        for (i = 0; i < (uint32_t)nn; i++) {
            if (!host_hosts(h, need[i])) {
                host_fwd_report(h, recs, count, irecs, ino_count, out,
                                need, nn);
                return;
            }
        }
    }
    pthread_mutex_lock(&h->read_mu);
    for (i = 0; i < count && rc == EFS_OK; i++) {
        sz = 0;
        for (j = 0; j < ino_count && irecs; j++) {
            if (irecs[j].ino == recs[i].ino) {
                sz = irecs[j].size;
                break;
            }
        }
        if (sz == 0)
            sz = ((uint64_t)recs[i].chunk_index + 1) * EFS_MIN_CHUNK_SIZE;
        rc = host_pub_locked(h, &recs[i], sz, &hint);
        if (env_on("EFS_RAFT_DBG"))
            fprintf(stderr, "raft-host: report pub ino=%llu ci=%u sz=%llu rc=%d\n",
                    (unsigned long long)recs[i].ino, recs[i].chunk_index,
                    (unsigned long long)sz, rc);
        /* INVAL is a permanent property of the rec (bad mode, zero node) —
         * it can never succeed, so skip it and keep publishing the rest of
         * the batch. One poison rec must not stall every rec behind it
         * (the client re-reports the skipped ino, so a misdiagnosed cause
         * resurfaces instead of being lost). Transient errors (NOT_PRIMARY,
         * NO_QUORUM, NET, BUSY, STALE) fail the batch for a client retry. */
        if (rc == EFS_ERR_INVAL) {
            fprintf(stderr,
                    "raft-host: report pub ino=%llu ci=%u INVAL, skipping rec\n",
                    (unsigned long long)recs[i].ino, recs[i].chunk_index);
            rc = EFS_OK;
            continue;
        }
        if (rc == EFS_OK) {
            int rrc = host_resolve_caught_up(h, recs[i].ino, sz, &hint);

            /* Reservation bookkeeping, not durability: the pub above
             * already made the data durable and visible (size rides the
             * lane MAX). A resolve failure defers cleanup to the next
             * report or the lease-close drain; failing the report here
             * would keep the rec dirty and poison every later sync
             * report. */
            if (rrc != EFS_OK)
                fprintf(stderr,
                        "raft-host: report resolve ino=%llu rc=%d (deferred)\n",
                        (unsigned long long)recs[i].ino, rrc);
        }
        if (rc != EFS_OK)
            fprintf(stderr,
                    "raft-host: report FAIL ino=%llu ci=%u rc=%d (phase=%s)\n",
                    (unsigned long long)recs[i].ino, recs[i].chunk_index, rc,
                    "pub");
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
}

void server_raft_host_getchunks(efs_ino_t ino, uint32_t start, uint32_t max,
                                struct efs_msg_inode_getchunks_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_chunk ch;
    uint32_t ci, group_end, lsh, seen = 0;
    uint8_t ig, lg;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (max == 0 || max > EFS_GETCHUNKS_MAX)
        max = EFS_GETCHUNKS_MAX;
    group_end = (start | (EFS_CHUNK_GROUP_SIZE - 1u)) + 1u;
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, ig)) {
        uint8_t need[2];
        host_need_both(need);
        host_fwd_getchunks(h, ino, start, max, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, ig, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && !host_holds_chunks(row.mode))
        rc = EFS_ERR_INVAL;
    seen = (rc == EFS_OK) ? (1u << ig) : 0;
    for (ci = start; rc == EFS_OK && ci < group_end && out->count < max; ci++) {
        uint8_t lane = (uint8_t)(ci % EFS_META_LANES);

        /* A chunk cannot exist on a lane the inode never registered.
         * ReadIndexing unused lanes would send this RPC to groups this
         * node does not host (fresh smoke: group-0 leader, lane 1+). */
        if ((row.active_lanes & (1ULL << lane)) == 0)
            continue;
        lsh = efs_kv_lane_shard(ino, lane);
        lg = efs_raft_shard_group(lsh);
        if (!host_hosts(h, lg)) {
            /* An active lane's group is not local: the chunk rows live
             * there, so this node cannot answer authoritatively. Bounce
             * to a dual-host (never self — no loop). */
            uint8_t need[2];
            int nn = 1;
            need[0] = ig;
            if (lg != ig)
                need[nn++] = lg;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_getchunks(h, ino, start, max, out, need, nn);
            return;
        }
        if ((seen & (1u << lg)) == 0) {
            int hh = -1;
            rc = host_read_index(h, lg, &hh);
            if (rc != EFS_OK)
                hint = hh;
            else
                seen |= 1u << lg;
        }
        if (rc != EFS_OK)
            break;
        rc = efs_meta_apply_get_chunk(h->kv, ino, ci, &ch);
        if (rc == EFS_ERR_NOT_FOUND) {
            rc = EFS_OK;
            continue;
        }
        if (rc != EFS_OK)
            break;
        out->recs[out->count].ino = ino;
        out->recs[out->count].chunk_index = ci;
        memcpy(out->recs[out->count].nodes, ch.nodes,
               sizeof(out->recs[out->count].nodes));
        memcpy(out->recs[out->count].checksums, ch.checksums,
               sizeof(out->recs[out->count].checksums));
        out->count++;
    }
    pthread_mutex_unlock(&h->read_mu);
    out->status = rc_to_inode_status(rc);
    out->primary_id = (hint >= 0) ? (efs_node_id_t)(hint + 1) : 0;
}

/* READDIR: ReadIndex the dir inode (and used dir-lane groups if HASHED
 * or SPLITTING), then scan. Apply merges LOCAL leftovers with hashed
 * lanes during SPLITTING (hashed side wins, I8). after_ino skips
 * already-returned inos so the old wire cursor still works. */
void server_raft_host_readdir(efs_ino_t parent, uint32_t max_ents,
                              uint32_t after_src, const char *after_name,
                              struct efs_msg_inode_readdir_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_dir_cursor cur;
    struct efs_meta_dir_ent page[EFS_READDIR_MAX];
    struct efs_meta_stat st;
    uint32_t got = 0, i;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (max_ents == 0 || max_ents > EFS_READDIR_MAX)
        max_ents = EFS_READDIR_MAX;
    {
        uint8_t pg = efs_raft_shard_group(efs_kv_inode_shard(parent));
        if (!host_hosts(h, pg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            if (pg != EFS_RAFT_GROUP_SHARD2)
                need[nn++] = EFS_RAFT_GROUP_SHARD2;
            else
                need[nn++] = EFS_RAFT_GROUP_SHARD;
            host_fwd_readdir(h, parent, max_ents, after_src, after_name, out,
                             need, nn);
            return;
        }
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(efs_kv_inode_shard(parent)),
                         &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &row);
    if (rc == EFS_OK && !S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && row.layout != EFS_META_LAYOUT_LOCAL) {
        uint32_t li;
        for (li = 0; li < EFS_META_LANES; li++) {
            uint8_t lg;
            if ((row.used_shards & (1ull << li)) == 0)
                continue;
            lg = efs_raft_shard_group(efs_kv_lane_shard(parent, (uint8_t)li));
            if (!host_hosts(h, lg)) {
                uint8_t need[2];
                int nn = 1;
                need[0] = efs_raft_shard_group(efs_kv_inode_shard(parent));
                if (lg != need[0])
                    need[nn++] = lg;
                pthread_mutex_unlock(&h->read_mu);
                host_fwd_readdir(h, parent, max_ents, after_src, after_name,
                                 out, need, nn);
                return;
            }
        }
    }
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, parent, &hint);
    /* Resume from the client's cookie: (src, name) is the exact KV scan
     * position. The scan is name-ordered, so this is stable and complete —
     * an ino filter would skip entries (name order != ino order). */
    memset(&cur, 0, sizeof(cur));
    if (after_name && after_name[0]) {
        cur.src = after_src;
        strncpy(cur.name, after_name, EFS_MAX_NAME - 1);
    }
    while (rc == EFS_OK && !cur.done && out->count < max_ents) {
        got = 0;
        /* Never scan past what fits in the reply: the cursor advances to
         * the last SCANNED name, so an unemitted entry would be skipped on
         * resume. */
        rc = efs_meta_apply_readdir(h->kv, parent, &cur, page,
                                    max_ents - out->count, &got);
        for (i = 0; i < got && rc == EFS_OK && out->count < max_ents; i++) {
            uint8_t cg;

            /* HASHED child's used_shards may be unhosted; stub like an
             * unhosted inode group instead of failing the whole listing. */
            cg = efs_raft_shard_group(efs_kv_inode_shard(page[i].d.ino));
            if (!host_hosts(h, cg)) {
            stub_ent:
                memset(&out->ents[out->count], 0, sizeof(out->ents[0]));
                out->ents[out->count].ino = page[i].d.ino;
                out->ents[out->count].mode = page[i].d.type;
                out->ents[out->count].parent = parent;
                strncpy(out->ents[out->count].name, page[i].name,
                        EFS_MAX_NAME - 1);
                out->count++;
                rc = EFS_OK;
                continue;
            }
            rc = host_read_inode_lanes(h, page[i].d.ino, &hint);
            if (rc == EFS_ERR_NOT_PRIMARY)
                goto stub_ent;
            if (rc != EFS_OK)
                break;
            rc = efs_meta_apply_getattr(h->kv, page[i].d.ino, host_txn_coord, h,
                                        &st);
            if (rc == EFS_ERR_NOT_PRIMARY)
                goto stub_ent;
            if (rc != EFS_OK)
                break;
            stat_to_inode(&st, &out->ents[out->count]);
            out->ents[out->count].parent = parent;
            strncpy(out->ents[out->count].name, page[i].name, EFS_MAX_NAME - 1);
            out->count++;
        }
    }
    /* Resume cookie: the cursor sits past the last emitted entry (or past
     * the last scanned one when a filter dropped it — either way nothing is
     * re-scanned or skipped). */
    out->next_src = cur.src;
    out->next_done = cur.done ? 1 : 0;
    strncpy(out->next_name, cur.name, EFS_MAX_NAME - 1);
    pthread_mutex_unlock(&h->read_mu);
    if (rc == EFS_OK)
        out->status = EFS_INODE_RPC_OK;
    else {
        out->count = 0;
        out->status = rc_to_inode_status(rc);
    }
}

/* LOOKUP_PATH: hop-by-hop ReadIndex + lookup, same as the in-sim walk.
 * Empty path is the start inode. Intermediate not-a-directory is INVAL.
 * A hop whose dentry or child inode lives on a group this node does not
 * host bounces, same as LOOKUP (scattered MKDIR dests). Intermediate
 * hops only need the inode row (layout/mode); do not ReadIndex a HASHED
 * ancestor's used_shards or a group-0-only replica fails the walk
 * before the hashed-dentry bounce. Leaf getattr bounces if lanes are
 * unhosted. */
void server_raft_host_lookup_path(efs_ino_t start, const char *path,
                                  struct efs_msg_inode_lookup_path_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    efs_ino_t cur, last_parent = 0;
    const char *p;
    uint32_t nh = 0;
    char last_name[EFS_MAX_NAME];
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    memset(last_name, 0, sizeof(last_name));
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    cur = start ? start : EFS_ROOT_INO;
    p = path ? path : "";
    while (*p == '/')
        p++;
    {
        uint8_t sg = efs_raft_shard_group(efs_kv_inode_shard(cur));
        if (!host_hosts(h, sg)) {
            uint8_t need[2];
            need[0] = EFS_RAFT_GROUP_SHARD;
            need[1] = EFS_RAFT_GROUP_SHARD2;
            host_fwd_lookup_path(h, start, path, out, need, 2);
            return;
        }
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_inode_lanes(h, cur, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        uint8_t need[2];

        pthread_mutex_unlock(&h->read_mu);
        host_need_both(need);
        host_fwd_lookup_path(h, start, path, out, need, 2);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, cur, &row);
    if (rc == EFS_OK && *p == '\0') {
        rc = efs_meta_apply_getattr(h->kv, cur, host_txn_coord, h, &st);
        pthread_mutex_unlock(&h->read_mu);
        set_inode_rc((struct efs_msg_inode_reply *)out, rc, hint);
        if (rc == EFS_OK)
            stat_to_inode(&st, &out->inode);
        return;
    }
    while (rc == EFS_OK && *p && nh < EFS_LOOKUP_PATH_MAX_DEPTH) {
        char name[EFS_MAX_NAME];
        size_t nlen;
        const char *s = p;
        uint32_t dsh;
        uint8_t dg, pg;

        while (*p && *p != '/')
            p++;
        nlen = (size_t)(p - s);
        while (*p == '/')
            p++;
        if (nlen == 0)
            break;
        if (nlen >= EFS_MAX_NAME) {
            rc = EFS_ERR_NAMETOOLONG;
            break;
        }
        memset(name, 0, sizeof(name));
        memcpy(name, s, nlen);
        if (!S_ISDIR(row.mode)) {
            rc = EFS_ERR_INVAL;
            break;
        }
        pg = efs_raft_shard_group(efs_kv_inode_shard(cur));
        dsh = efs_kv_dentry_shard(cur, name, row.layout);
        dg = efs_raft_shard_group(dsh);
        if (dg != pg && !host_hosts(h, dg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            need[nn++] = dg;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_lookup_path(h, start, path, out, need, nn);
            return;
        }
        if (dg != pg)
            rc = host_read_index(h, dg, &hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_lookup(h->kv, cur, name, &dent);
        if (rc == EFS_OK) {
            uint8_t cg = efs_raft_shard_group(efs_kv_inode_shard(dent.ino));
            if (!host_hosts(h, cg)) {
                uint8_t need[2];
                int nn = 1;
                need[0] = pg;
                if (cg != pg)
                    need[nn++] = cg;
                pthread_mutex_unlock(&h->read_mu);
                host_fwd_lookup_path(h, start, path, out, need, nn);
                return;
            }
        }
        if (rc == EFS_OK)
            rc = efs_meta_apply_get_inode(h->kv, dent.ino, &row);
        if (rc == EFS_OK) {
            last_parent = cur;
            memcpy(last_name, name, EFS_MAX_NAME);
            out->ancestors[nh].ino = row.ino;
            out->ancestors[nh].mode = row.mode;
            out->ancestors[nh].uid = row.uid;
            out->ancestors[nh].gid = row.gid;
            cur = row.ino;
            nh++;
        }
    }
    if (rc == EFS_OK && *p)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && nh > 0) {
        out->ancestor_count = nh > 1 ? nh - 1 : 0;
        rc = host_read_inode_lanes(h, cur, &hint);
        if (rc == EFS_ERR_NOT_PRIMARY) {
            uint8_t need[2];

            pthread_mutex_unlock(&h->read_mu);
            host_need_both(need);
            host_fwd_lookup_path(h, start, path, out, need, 2);
            return;
        }
        if (rc == EFS_OK)
            rc = efs_meta_apply_getattr(h->kv, cur, host_txn_coord, h, &st);
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc((struct efs_msg_inode_reply *)out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = last_parent;
        strncpy(out->inode.name, last_name, EFS_MAX_NAME - 1);
    }
}

