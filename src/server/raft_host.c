#include "server_internal.h"
#include "efs/checksum.h"
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/kv_snap.h"
#include "efs/meta_apply.h"
#include "efs/publication.h"
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
#include <stddef.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "efs/rdma.h"

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
/* Per-peer outbound queue cap. A full queue used to drop the newest
 * message and still return success, so ae_inflight was set for an
 * AppendEntries that was never queued and the follower stopped
 * applying until the heartbeat. host_send evicts an older heartbeat
 * or reply, keeps snapshot chunks and entry-carrying AppendEntries
 * in order, and returns EFS_ERR_AGAIN when the new message itself
 * cannot be queued. Bumped 256 -> 2048 after hi=256 + drops put
 * fcstor005 thousands of entries behind. */
#define HOST_OUTBOX_MAX    2048
#define HOST_ENCODE_STACK  (64 * 1024)
#define HOST_NGROUPS       2
#define HOST_READ_TRIES    80 /* 80 × 5 ms = 400 ms; heartbeat is 50 ms */
#define HOST_CREATE_NAME_OFF 31
static void host_hold_uuid(uint64_t owner, uint8_t uuid[EFS_OPID_UUID_LEN]);
static int pack_lease_cmd(uint8_t *out, uint32_t *len, int open, efs_ino_t ino,
                          uint64_t gen, const uint8_t uuid[EFS_OPID_UUID_LEN],
                          uint32_t epoch);
#define HOST_CMD_MAX       512 /* stack size for small cmds, not a wire cap */
#define HOST_SETATTR_LEN   61
#define HOST_XATTR_HDR     28
#define HOST_UTIMENS_LEN   73
/* TRUNCATE: +8 lane_mask (which active lanes THIS entry fences — the ones
 * whose shard maps to the inode's group) +1 flags (bit0: tail_external —
 * the tail chunk's lane is on another group and is published separately
 * after commit). Cross-group lanes were fenced by LANE_FENCE entries on
 * their own groups before this entry was proposed. */
#define HOST_TRUNC_LEN     63
#define HOST_TRUNC_F_TAIL_EXT 1
/* Bit1: unaligned size, no tail CAS. The tail chunk is a generation-0
 * span row; a zero stub would drop the trailer and the bytes with it.
 * The client folds spans into one image before truncate when it can.
 * This flag is the case where that fold has not landed yet. */
#define HOST_TRUNC_F_KEEP_TAIL 2
#define HOST_APPEND_RSV_LEN 45
#define HOST_APPEND_RES_LEN 38
#define HOST_TRUNC_TAIL    (4u + 8u + 8u + 4u + \
                            (uint32_t)EFS_NUM_FRAGMENTS * (4u + EFS_HASH_SIZE))
/* PUBLISH: +8 inode_gen +8 mtime_gen +1 flags (bit0: lane_local — applied
 * on the lane's group, whose KV has no inode row; see meta_apply.h). */
#define HOST_PUBLISH_LEN   (29u + (uint32_t)EFS_NUM_FRAGMENTS * (4u + EFS_HASH_SIZE) + \
                            EFS_OPID_UUID_LEN + 4u + 8u + 8u + 8u + 4u + \
                            8u + 8u + 1u)
/* Many publications per Raft proposal (data.md §7.3). 2048 × ~202 B ≈
 * 404 KiB, under one AE (1 MiB). N=4096 did not beat 2048 (entry size
 * ate the fewer-propose win). Shrinking the batch so the frame fit a
 * 72 KiB RDMA recv made the 9-client dd wall worse (more proposals).
 * Report fsyncs are amortized by efs_raft_disk_sync_hold around the
 * pack/push loop. */
#define HOST_PUB_BATCH_N   2048u
#define HOST_PUB_F_LANE_LOCAL 1
#define HOST_PUB_F_FRESH_OBJECT 2
#define HOST_PUBLICATION_LEN (HOST_PUBLISH_LEN + 28u)
#define HOST_PUB_TAIL_TRIES  4 /* cross-group truncate tail CAS retries */
#define HOST_ACTIVATE_LANE_LEN 10 /* tag + ino:8 + lane:1 */
#define HOST_ACTIVATE_MASK_LEN 17 /* tag + ino:8 + mask:8 */
#define HOST_LANE_FENCE_LEN    39 /* tag + ino:8 + gen:8 + lane:1 +
                                   * epoch:8 + size:8 + tail_ci:4 +
                                   * has_tail:1 — matches meta_cmd.h */
#define HOST_DIR_LEN       10
#define HOST_CFG_LEN       6 /* tag + sub + voters:4 */
#define HOST_SPREAD_MAX    8 /* leftovers per GC tick; batch, not a scan */
/* Log-truncation batch. The export is a file shipped as SNAP chunks;
 * this is only how often the snap point advances. */
#define HOST_SNAP_MIN      256
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
    /* Per-peer wakeup (guarded by h->outbox_mu). Until Sep 20 every sender
     * waited on ONE shared h->outbox_cv and host_send used cond_signal, so
     * a message queued for peer A routinely woke the sender for peer B,
     * which found its own queue empty and went back to sleep; A's sender
     * ran only at the next signal for anyone — the next 50 ms heartbeat.
     * Every AppendEntries and every AE reply therefore waited 0–50 ms on
     * each hop, and a Raft commit took ~100 ms instead of ~1 ms: mkdir
     * median 103 ms, create+close 60–107, O_APPEND+close 160–180, with the
     * fsyncs at 0.3 ms. Visible in strace as followers acking only every
     * other heartbeat, two replies back to back. */
    pthread_cond_t cv;
    /* Private connection to the peer, outside server_peer_conn_get's
     * bounded pool (see server_peer_conn_new): consensus traffic never
     * queues behind forwarded client commands. Owned by the sender
     * thread only; reconnected on the next message after an error. */
    struct efs_conn *conn;
    /* W14.2: two lanes. pri is heartbeats, AE replies, votes; ent is
     * entry-carrying AppendEntries and snapshot chunks, in order.
     * The sender drains pri first so a full entry lane cannot hide a
     * heartbeat. Depth is npri + nent, capped at HOST_OUTBOX_MAX. */
    int npri;
    int nent;
    struct host_outbox_item pri[HOST_OUTBOX_MAX];
    struct host_outbox_item ent[HOST_OUTBOX_MAX];
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

#define HOST_FIN_MAX 1024
struct host_fin_item {
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint32_t coord;
    uint64_t born_ns;
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
    uint32_t desired; /* operator target; actual follows via joint (I18) */
    uint64_t applied_saved;
    uint64_t applied_saved_us; /* pump-only: last applied-file write */
    int applied_fd;            /* pump-only: applied.<g>, held open; -1 */
    /* Snapshot file for this group. The pump pins a view and the GC
     * thread writes snap-<group>-<incl>.kvx. part_* is the follower's
     * in-progress InstallSnapshot. fds are -1 when closed. */
    int snap_exporting;
    int snap_ready;
    int snap_fd;
    uint64_t snap_incl;
    uint64_t snap_bytes;
    struct efs_kv_lsm_view *snap_view;
    char snap_path[EFS_MAX_PATH];
    int part_fd;
    uint64_t part_incl;
    uint64_t part_term;
    uint64_t part_off;
    char part_path[EFS_MAX_PATH];
    /* InstallSnapshot apply. The pump writes the .part file; the GC
     * thread scans and diffs it. import_state is snap_mu. A retry of the
     * last chunk returns BUSY until DONE, then OK. import_gen bumps when
     * a newer snapshot replaces this one. */
    int import_state;
    int import_rc;
    /* 1 while this group's install diff is in flight. host_apply of this
     * group returns BUSY. The other group keeps applying: the scan is a
     * pinned view and does not hold the LSM lock. */
    int import_block;
    uint64_t import_gen;
    uint64_t import_incl;
    uint64_t import_term;
    char import_path[EFS_MAX_PATH];
    /* Prepared diff. Key pointers live in the two hold buffers. The pump
     * batches this in the same call that acknowledges the snapshot, so a
     * newer snapshot can still discard it without writing the KV. */
    struct efs_kv_item *import_diff;
    uint32_t import_ni;
    uint32_t import_at;      /* keys already batched; slice cursor */
    int import_applying;     /* pump holds the diff; cancel must not free */
    uint8_t *import_hold_a;
    uint8_t *import_hold_b;
    uint64_t import_retry_us;
    int kv_incomplete;  /* apply missed a committed row; do not lead */
    struct efs_raft *r;
    struct efs_raft_host *host; /* back-pointer, set in attach_group */
    /* Apply-result ring: (index, rc, extra) of the last HOST_APPLY_RC_RING
     * applied entries, written by host_apply under h->mu BEFORE the raft
     * core bumps last_applied. Lets host_propose_wait return the apply
     * layer's verdict (a rejected txn PREP: intent-conflict BUSY / version
     * STALE) instead of only "the index applied". Without it a lost
     * conflict looked like success and a cross-shard txn committed with a
     * partial intent set — the peer_concurrent_hardlink nlink lost-update.
     * extra is the O_APPEND reserved offset for APPEND_RSV (not the live
     * watermark — two concurrent reserves otherwise both write at wm-len).
     * arc_term is the applied entry's term: a proposer matches (index,
     * term), never the index alone. A leader that loses its term has its
     * uncommitted tail truncated and a DIFFERENT entry lands at the same
     * index; a waiter keyed by index read that stranger's OK verdict as
     * its own. For a txn coordinator that was "DECIDE COMMIT applied" →
     * RESOLVE COMMIT on one participant while the decision never existed
     * → recovery ABORTed the rest = the half-applied rmdir/unlink rows of
     * Sep 21 (I17: child row gone, dentry + parent counts kept). */
    uint64_t arc_idx[HOST_APPLY_RC_RING];
    uint64_t arc_term[HOST_APPLY_RC_RING];
    int32_t arc_rc[HOST_APPLY_RC_RING];
    uint64_t arc_extra[HOST_APPLY_RC_RING];
    /* EFS_RAFT_OBS: last logged term/role, for election-churn timing. */
    uint64_t obs_term;
    int obs_role;
    /* Lock-free view of the replica for RPC handlers, published by
     * host_publish_view under h->mu (pump: end of every cycle; also right
     * after a read_begin/propose) and read with __atomic_load_n. Handlers
     * used to take h->mu just to read commit/applied/leader and to
     * cond_wait on applied_cv, so under the 9-host posix suite 30+ RPC
     * threads per cycle queued on the pump's own mutex, the pump lost its
     * 50 ms heartbeat cadence and both groups re-elected four times in
     * 50 s (results/measure/20260922-*-w8-stall-timeline). h->mu is now
     * taken by a handler only to mutate raft (propose, read_begin). */
    uint64_t v_commit, v_applied, v_read_index, v_term;
    int v_has;          /* 1 when a local replica exists */
    int v_role, v_leader;
    int v_read_pending, v_read_done;
    uint64_t v_stamp;   /* bumped by every publish that changed a field */
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

/* One sleeping handler (see the waiters[] comment in efs_raft_host). All
 * fields are read and written under cv_mu. */
#define HOST_WAITERS 512
struct host_waiter {
    pthread_cond_t cv;
    uint64_t idx;   /* wake once the group's applied >= idx */
    uint64_t seen;  /* any=1: wake once the group's view stamp != seen */
    uint8_t group;
    uint8_t active;
    uint8_t any;
};

struct efs_raft_host {
    struct efsd_server *s;
    int raft_id;
    int n;
    uint64_t boot_id;
    uint64_t salt;
    /* Per-export MKDIR scatter salt (architecture.md §7.4). Cached after
     * the first successful KV read — it is written once at raft-mkfs and
     * never changes. Skipping the ROOT ReadIndex on every mkdir is the
     * difference between a 10 ms same-group mkdir and a 60 ms one. */
    uint64_t export_salt;
    int export_salt_valid;
    struct efs_kv *kv;
    struct efs_raft_disk *disk;
    char mdraft[EFS_MAX_PATH];
    struct host_group g[HOST_NGROUPS];
    pthread_mutex_t mu;
    pthread_mutex_t snap_mu; /* snapshot file/view; never taken under h->mu
                              * from the GC thread. The pump may hold h->mu
                              * then snap_mu. */
    /* 1 while a snapshot diff batch holds the LSM lock. Every group's
     * apply returns BUSY so the pump does not block behind that batch. */
    int snap_batch;
    /* No host-wide handler lock: ReadIndex rounds are batched under h->mu
     * (host_read_index) and the apply is the arbiter of every check-then-
     * propose (guards, CAS, reductions). The old read_mu that every inode
     * handler held across its whole KV read + propose serialized one
     * metadata RPC per leader (W8, Sep 21). */
    pthread_mutex_t wait_mu; /* lock wait queue; never held while sleeping */
    struct host_lock_wait *wait_head;
    struct host_lock_wait *wait_tail;
    pthread_t tid;
    int running;
    int started;
    pthread_t gc_tid;    /* background GC reaper (spec L7) */
    int gc_running;
    int gc_started;
    /* Stranded-txn recovery (L5). rec_mark[shard] is the time of the first
     * PREPARE applied on that shard since it was last scanned clean (0 =
     * nothing pending was ever written there). The GC pass scans only
     * shards whose mark is HOST_REC_AGE_NS old, so an idle cluster scans
     * nothing and a busy shard is scanned once per age period. Written by
     * the apply path (under h->mu), read by the GC thread — rec_mu. */
    pthread_mutex_t rec_mu;
    uint64_t rec_mark[EFS_KV_SHARD_MASK + 1];
    uint64_t rec_found, rec_aborted, rec_resolved, rec_fail, rec_scans;
    /* Txn finisher: a coordinator whose DECIDE COMMIT is in the log but
     * whose apply wait ran out (or whose RESOLVEs did not all land) hands
     * the txn here instead of leaving its intents for the 5 s recovery
     * scan. Ring under fin_mu; fin_cv wakes the thread. */
    pthread_t fin_tid;
    int fin_running, fin_started;
    pthread_mutex_t fin_mu;
    pthread_cond_t fin_cv;
    struct host_fin_item *fin;
    uint32_t fin_head, fin_n;
    uint64_t fin_queued, fin_done, fin_dropped, fin_full;
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
    /* applied_cv waiters (host_wait_applied, host_read_index) sleep under
     * cv_mu, not h->mu: their predicate is the published view above, and
     * the pump takes cv_mu only for the broadcast. */
    pthread_mutex_t cv_mu;
    pthread_cond_t applied_cv;
    /* Targeted wakeups. A broadcast on applied_cv woke every waiting
     * handler on every pump cycle: under 144 concurrent mkdirs that was
     * ~150 threads x hundreds of cycles/s of futex wake + wait, all on one
     * futex word, and the futex hash-bucket spinlock plus cv_mu ping-pong
     * was ~20 % of efsd samples (perf, Sep 26). A waiter now registers the
     * index it needs (or "any change of this group's view" for a
     * read-index round) in a slot with its own condvar, and the pump
     * signals only the slots whose predicate the fresh view satisfies.
     * applied_cv stays as the overflow path when every slot is taken. */
    struct host_waiter waiters[HOST_WAITERS];
    uint32_t waiter_hint;
    uint32_t cv_overflow; /* waiters sleeping on applied_cv (under cv_mu) */
    /* Per-peer send outboxes. host_send runs under h->mu (pump ticks and
     * proposer threads) and must NEVER do network I/O there: a dead peer's
     * synchronous send+ACK (up to HOST_SEND_IO_MS) under h->mu stalled
     * every election tick and proposal on BOTH groups — that was the
     * election churn under load (term 46 observed). host_send now only
     * encodes and queues; one lazily-spawned sender thread per peer does
     * the blocking send + empty-ACK wait, preserving per-peer FIFO order,
     * dead-conn detection and per-peer backpressure. A dead peer stalls
     * only its own sender. outbox_mu guards all tx[] state, each sender
     * sleeps on its own tx[].cv (see struct host_outbox);
     * tx_running=0 tells senders to drop-and-exit and host_send to drop. */
    pthread_mutex_t outbox_mu;
    int tx_running;
    struct host_outbox tx[EFS_RAFT_MAX_PEERS];
    uint8_t orphan_cursor[HOST_NGROUPS][11];
    uint32_t orphan_cursor_len[HOST_NGROUPS];
    uint8_t orphan_discovery_cursor[HOST_NGROUPS][EFS_KV_KEY_MAX];
    uint32_t orphan_discovery_len[HOST_NGROUPS];
    int orphan_discovery_done[HOST_NGROUPS];
    /* EFS_RAFT_OBS: wait_applied timeouts, pump h->mu hold high-water, and
     * the last stats-dump timestamp (ms). */
    uint64_t obs_wait_timeouts;
    /* Raft frames a peer handler could not queue (inbox full): the frame
     * is lost and the peer still gets its RAFT_REPLY. Counted since Sep 30
     * 2026; before that a drop had no line and no counter. Not gated on
     * EFS_RAFT_OBS — the first ten are logged, the total is printed on
     * the raft-obs line. */
    uint64_t inbox_drop;
    /* Last 64 publish-batch waits (propose → apply), microseconds. */
    uint64_t obs_pub_ring[64];
    uint64_t obs_pub_n;
    uint64_t obs_pub_max_us;
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
    /* Apply-result ring reads that found the slot overwritten
     * (>HOST_APPLY_RC_RING applies in one wait window). The waiter returns
     * BUSY (verdict unknown), so a sustained nonzero value means the ring
     * is too small. obs_arc_term_miss: slot holds idx under another term =
     * the proposer's entry was truncated by a leader change; the waiter
     * returns NOT_PRIMARY. Expected to be nonzero only across elections. */
    uint64_t obs_arc_miss;
    uint64_t obs_arc_term_miss;
    /* I16: directory RPCs answered from the op-id window (a client retry
     * of an op that had already committed). Handler threads, atomic. */
    uint64_t obs_opid_replay;
    /* Set by apply_append_rsv_cmd; host_apply copies it into the ring. */
    uint64_t apply_extra;
    /* In-memory I16 for APPEND: a retried RPC with the same (ino,token,seq)
     * must not propose a second reservation (that left a 6-byte hole).
     * Not in the Raft cmd — 400 unique seqs in the op-id window is NOMEM. */
    struct {
        uint64_t token;
        uint64_t seq;
        efs_ino_t ino;
        uint64_t off;
    } ap_rep[512];
    uint32_t ap_rep_i;
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

/* Op-id trailer on the log-path directory commands (I16): the LAST
 * HOST_OPID_TRAILER bytes of the command, big-endian, present when the
 * command says so (CREATE: bit 0 of cmd[1]; UNLINK/RMDIR: by length). The
 * uuid+epoch slot after the name stays what it was — for CREATE it is the
 * lease holder's identity, not the requester's. */
#define HOST_CREATE_F_OPID 0x01
#define HOST_OPID_TRAILER (EFS_OPID_UUID_LEN + 4u + 8u + 8u)

static uint32_t pack_opid_trailer(uint8_t *p, const struct efs_opid_req *q)
{
    if (!q || !efs_opid_req_valid(q))
        return 0;
    memcpy(p, q->id.client_uuid, EFS_OPID_UUID_LEN);
    wr32be(p + EFS_OPID_UUID_LEN, q->id.session_epoch);
    wr64be(p + EFS_OPID_UUID_LEN + 4, q->id.seq);
    wr64be(p + EFS_OPID_UUID_LEN + 12, q->ack);
    return HOST_OPID_TRAILER;
}

static void unpack_opid_trailer(const uint8_t *p, struct efs_opid_req *q)
{
    memset(q, 0, sizeof(*q));
    memcpy(q->id.client_uuid, p, EFS_OPID_UUID_LEN);
    q->id.session_epoch = rd32be(p + EFS_OPID_UUID_LEN);
    q->id.seq = rd64be(p + EFS_OPID_UUID_LEN + 4);
    q->ack = rd64be(p + EFS_OPID_UUID_LEN + 12);
}

static uint64_t now_ns(void);
static void host_stop_senders(struct efs_raft_host *h);
static void host_rec_mark(struct efs_raft_host *h, uint32_t shard);

static int env_on(const char *name)
{
    const char *v = getenv(name);
    if (!v || !v[0] || strcmp(v, "0") == 0)
        return 0;
    return 1;
}

/* Success-path apply lines used to hit stderr on every CREATE/SESSION/
 * SETATTR. Under posix that is thousands of sync writes on the pump
 * thread (mkdir was ~250 ms). Errors stay unconditional. */
static int raft_dbg_on(void)
{
    static int v = -1;
    if (v < 0)
        v = env_on("EFS_RAFT_DBG");
    return v;
}

#define APPLY_LOG(...) do { if (raft_dbg_on()) fprintf(stderr, __VA_ARGS__); } while (0)

static uint64_t now_us_(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* A directory op that ends BUSY or STALE is normal under contention and
 * must never be persistent on an idle cluster (Sep 21: one of nine
 * fresh-parent mkdirs ate the client's whole 10.4 s retry budget with no
 * server line to say why). Log those two at most 20 lines per second.
 * EXIST / NOT_FOUND / NOT_EMPTY are the op's real answer (EXIST is every
 * O_CREAT of a name that is already there) and stay quiet. Anything else
 * (IO, INVAL, PROTO, NOT_PRIMARY, …) was silent and is how the 20:21Z
 * 9-host creates failed with no server line — log those always. */
static int dirop_fail_on(int rc)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t win_us;
    static unsigned n;
    uint64_t now;
    int ok;

    if (rc == EFS_OK || rc == EFS_ERR_EXIST || rc == EFS_ERR_NOT_FOUND ||
        rc == EFS_ERR_NOT_EMPTY)
        return 0;
    if (raft_dbg_on())
        return 1;
    if (rc != EFS_ERR_BUSY && rc != EFS_ERR_STALE)
        return 1;
    now = now_us_();
    pthread_mutex_lock(&mu);
    if (now - win_us >= 1000000ull) {
        win_us = now;
        n = 0;
    }
    ok = n < 20;
    if (ok)
        n++;
    pthread_mutex_unlock(&mu);
    return ok;
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

/* Same rule as raft.c valid_voters: RF = 2f+1, so the popcount is odd. */
static int cfg_voters_ok(uint32_t v)
{
    int n = 0, i;

    if (v == 0 || (v >> EFS_RAFT_MAX_PEERS))
        return 0;
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (v & (1u << i))
            n++;
    }
    return n >= 1 && (n % 2) == 1;
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

/* Frames one sender keeps unacked on the wire. TCP has socket buffers;
 * RDMA is bounded by the peer's posted recv buffers (efs_rdma_conn_nrecv,
 * both ends post the same count), minus one the peer may still be
 * consuming. Never more than the pri + ent lanes can hold. */
#define HOST_SENDER_PIPE_MAX 8

static int host_sender_depth(struct efs_conn *pc)
{
    int d = HOST_SENDER_PIPE_MAX;

    if (pc && pc->kind == EFS_CONN_RDMA && pc->rc) {
        int n = efs_rdma_conn_nrecv(pc->rc) - 1;

        if (n < d)
            d = n;
    }
    return d < 1 ? 1 : d;
}

/* Per-peer sender: pops encoded messages FIFO and does the blocking
 * send + empty-ACK wait (dead-conn detection + backpressure) that used to
 * run under h->mu. Never touches h->mu; outbox_mu is never held across
 * I/O. On stop (tx_running=0) it drops whatever is queued and exits — the
 * server is going away and Raft retries from the next incarnation.
 *
 * One frame per round trip was the commit latency (Sep 29): the peer's
 * handler answers RAFT_REPLY at once, so the ~440 us rtt was pure wire +
 * wakeup, and every ReadIndex round on the leader put two heartbeats in
 * the lane ahead of the next entry AE. The lane reached 651 deep and a
 * one-entry commit waited 20-149 ms (`apply-sleep`); 400 ms of it was a
 * BUSY REPORT. Now a wake drains up to host_sender_depth frames, sends
 * them back to back, then collects that many acks in order. The peer
 * receives them on one conn and copies each into its inbox in order, so
 * ordering within a lane is unchanged. */
static void *host_sender(void *arg)
{
    struct host_outbox *tx = arg;
    struct efs_raft_host *h = tx->h;

    for (;;) {
        uint8_t *bufs[HOST_SENDER_PIPE_MAX];
        uint32_t lens[HOST_SENDER_PIPE_MAX];
        char host[64];
        uint16_t port = 0;
        struct efs_conn *pc;
        int i, n = 0, depth, sent = 0, acked = 0, bad = 0;
        uint64_t t0 = 0, t1 = 0;
        int obs = raft_obs_on();

        pthread_mutex_lock(&h->outbox_mu);
        while (tx->npri == 0 && tx->nent == 0 && h->tx_running)
            pthread_cond_wait(&tx->cv, &h->outbox_mu);
        if (!h->tx_running) {
            for (i = 0; i < tx->npri; i++)
                free(tx->pri[i].buf);
            for (i = 0; i < tx->nent; i++)
                free(tx->ent[i].buf);
            tx->npri = 0;
            tx->nent = 0;
            pthread_mutex_unlock(&h->outbox_mu);
            if (tx->conn) {
                efs_conn_destroy(tx->conn);
                tx->conn = NULL;
            }
            return NULL;
        }
        /* The depth depends on the conn, which only this thread touches;
         * a conn that is not up yet is sized at one frame. */
        depth = tx->conn ? host_sender_depth(tx->conn) : 1;
        /* Heartbeats and replies go out before entry AppendEntries.
         * Each lane stays FIFO. */
        while (n < depth && tx->npri > 0) {
            bufs[n] = tx->pri[0].buf;
            lens[n] = tx->pri[0].len;
            memmove(&tx->pri[0], &tx->pri[1],
                    (size_t)(tx->npri - 1) * sizeof(tx->pri[0]));
            tx->npri--;
            n++;
        }
        while (n < depth && tx->nent > 0) {
            bufs[n] = tx->ent[0].buf;
            lens[n] = tx->ent[0].len;
            memmove(&tx->ent[0], &tx->ent[1],
                    (size_t)(tx->nent - 1) * sizeof(tx->ent[0]));
            tx->nent--;
            n++;
        }
        pthread_mutex_unlock(&h->outbox_mu);

        if (obs)
            t0 = now_us_();
        if (peer_addr(h, tx->peer, host, sizeof(host), &port) != 0) {
            for (i = 0; i < n; i++)
                free(bufs[i]);
            continue;
        }
        if (!tx->conn)
            tx->conn = server_peer_conn_new(host, port);
        pc = tx->conn;
        if (!pc) {
            tx->st_fail += (uint64_t)n;
            for (i = 0; i < n; i++)
                free(bufs[i]);
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
        /* Every conn kind. Until Sep 30 2026 this was TCP-only, so an
         * RDMA lane waited EFS_IO_TIMEOUT_MS (30 s) for one RAFT_REPLY
         * while every heartbeat behind it sat in the queue; the unheard
         * peer campaigned and deposed the leader (426 group-2 terms in
         * 13 min under one ecopy, results/measure/20260930-040600-*). */
        efs_conn_set_recv_timeout(pc, HOST_SEND_IO_MS);
        if (pc->fd >= 0)
            efs_set_send_timeout(pc->fd, HOST_SEND_IO_MS);
        /* A fresh conn was sized at 1 above; RDMA may allow fewer than
         * the frames popped if the conn changed kind. Send what fits. */
        depth = host_sender_depth(pc);
        {
            /* A frame larger than the RDMA buffer (72 KiB) goes over the
             * conn's TCP side-channel and its RAFT_REPLY comes back on
             * TCP; the rest are answered on RDMA. efs_conn_send_msg
             * leaves recv_chan at the LAST message's channel, so a batch
             * of [AE_REP (RDMA), big AE (TCP)] used to read TCP only,
             * leave the RDMA reply in the ring, and time out — every
             * batch, on the one lane that mixes the two (the dual-group
             * hosts, fcstor004<->fcstor005: one group's AppendEntries
             * plus the other group's replies). Under a 100 GiB dd the
             * publish batches made every AE big, the lane failed at
             * HOST_SEND_IO_MS forever (fail=2399 in 10 min), and the
             * follower it fed sat 700 entries behind while every
             * follower-served read returned BUSY (Sep 30 2026,
             * results/measure/20260930-060000-dd-wedge). Count the
             * expected replies per channel and read each on its own. */
            int n_rdma = 0, n_tcp = 0;

            for (sent = 0; sent < n && sent < depth; sent++) {
                if (efs_conn_send_msg(pc, EFS_MSG_RAFT, bufs[sent],
                                      lens[sent]) != 0) {
                    bad = 1;
                    break;
                }
                if (pc->recv_chan == EFS_CONN_RDMA)
                    n_rdma++;
                else
                    n_tcp++;
            }
            for (acked = 0; acked < sent && !bad; acked++) {
                uint8_t rtype = 0;
                void *reply = NULL;
                uint32_t rlen = 0;

                /* With replies pending on both, the RDMA wait drains the
                 * ring first and falls through to TCP when a byte is
                 * there; with only TCP pending, read TCP directly. */
                pc->recv_chan = n_rdma > 0 ? EFS_CONN_RDMA : EFS_CONN_TCP;
                if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0 ||
                    rtype != EFS_MSG_RAFT_REPLY) {
                    free(reply);
                    bad = 1;
                    break;
                }
                free(reply);
                if (pc->last_recv_chan == EFS_CONN_RDMA) {
                    if (n_rdma == 0) {
                        bad = 1; /* a reply we did not send for */
                        break;
                    }
                    n_rdma--;
                } else {
                    if (n_tcp == 0) {
                        bad = 1;
                        break;
                    }
                    n_tcp--;
                }
            }
        }
        if (bad) {
            /* Everything unacked is lost with the conn; Raft resends. */
            tx->st_fail += (uint64_t)(n - acked);
            efs_conn_destroy(pc);
            tx->conn = NULL;
        } else if (sent < n) {
            /* Popped more than this conn may carry: the rest were not
             * sent. Raft retries them (heartbeat / next AE). */
            tx->st_fail += (uint64_t)(n - sent);
        }
        for (i = 0; i < n; i++)
            free(bufs[i]);
        if (obs) {
            tx->st_sent += (uint64_t)acked;
            if (acked)
                tx->st_rtt_us += now_us_() - t1;
        }
    }
}

/* nentries is the last big-endian u32 of the raft header. -1 if the
 * buffer is too short to say. */
static int outbox_nentries(const uint8_t *buf, uint32_t len)
{
    if (!buf || len < EFS_WIRE_RAFT_HDR_LEN)
        return -1;
    return (int)(((uint32_t)buf[EFS_WIRE_RAFT_HDR_LEN - 4] << 24) |
                 ((uint32_t)buf[EFS_WIRE_RAFT_HDR_LEN - 3] << 16) |
                 ((uint32_t)buf[EFS_WIRE_RAFT_HDR_LEN - 2] << 8) |
                 (uint32_t)buf[EFS_WIRE_RAFT_HDR_LEN - 1]);
}

/* Entry-carrying AppendEntries and snapshot chunks stay in order on
 * the entry lane. A heartbeat is an empty AE_REQ. */
static int outbox_entry_lane(const uint8_t *buf, uint32_t len)
{
    if (!buf || len < 1)
        return 0;
    if (buf[0] == EFS_RAFT_MSG_SNAP_REQ)
        return 1;
    return buf[0] == EFS_RAFT_MSG_AE_REQ && outbox_nentries(buf, len) > 0;
}

static int outbox_is_heartbeat(const uint8_t *buf, uint32_t len)
{
    return buf && buf[0] == EFS_RAFT_MSG_AE_REQ &&
           outbox_nentries(buf, len) == 0;
}

static int outbox_is_ae_rep(const uint8_t *buf, uint32_t len)
{
    return buf && len >= 1 && buf[0] == EFS_RAFT_MSG_AE_REP;
}

/* Replace the first priority-lane item matching pred. The newest
 * heartbeat / AE reply is the only one that matters. */
static int outbox_pri_replace(struct host_outbox *tx,
                              int (*pred)(const uint8_t *, uint32_t),
                              uint8_t *buf, uint32_t len)
{
    int i;

    for (i = 0; i < tx->npri; i++) {
        if (!pred(tx->pri[i].buf, tx->pri[i].len))
            continue;
        free(tx->pri[i].buf);
        tx->pri[i].buf = buf;
        tx->pri[i].len = len;
        return 1;
    }
    return 0;
}

/* Called under h->mu from the pump (tick) and from proposer threads
 * (efs_raft_propose), so it must NEVER do network I/O: it only encodes and
 * queues to the destination peer's outbox; the per-peer sender thread does
 * the blocking send. W14.2: an AE reply replaces any older AE reply to
 * this peer still queued, and a heartbeat replaces an older heartbeat;
 * a vote is never evicted. Only when the outbox is full of messages
 * that cannot be replaced does this return EFS_ERR_AGAIN (no drop).
 * send_ae turns that into success: the entry is already in the leader
 * log, and the next tick retries. */
/* Fill the fixed header of a buffer whose entry bytes are already at
 * EFS_WIRE_RAFT_HDR_LEN (send_ae). */
static void host_fill_ae_hdr(uint8_t *p, const struct efs_raft_msg *msg)
{
    uint8_t *q = p;

    q[0] = msg->type;
    q[1] = msg->group;
    q[2] = (uint8_t)(msg->vote_granted ? 1 : 0);
    q[3] = (uint8_t)(msg->success ? 1 : 0);
    q += 4;
    wr32be(q, (uint32_t)msg->from); q += 4;
    wr32be(q, (uint32_t)msg->to); q += 4;
    wr64be(q, msg->term); q += 8;
    wr64be(q, msg->boot_id); q += 8;
    wr64be(q, msg->last_log_index); q += 8;
    wr64be(q, msg->last_log_term); q += 8;
    wr64be(q, msg->prev_index); q += 8;
    wr64be(q, msg->prev_term); q += 8;
    wr64be(q, msg->leader_commit); q += 8;
    wr64be(q, msg->match_index); q += 8;
    wr32be(q, msg->nentries);
}

static void host_wire_taken(const struct efs_raft_msg *msg)
{
    if (msg && msg->wire)
        ((struct efs_raft_msg *)msg)->wire = NULL;
}

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
    if (msg->wire && msg->wire_len >= EFS_WIRE_RAFT_HDR_LEN) {
        host_fill_ae_hdr(msg->wire, msg);
        buf = msg->wire;
        len = msg->wire_len;
        heap = msg->wire;
        rc = EFS_OK;
    } else
        rc = efs_wire_raft_encode(msg, buf, cap, &len);
    if (rc == EFS_ERR_NOMEM) {
        uint32_t i;
        cap = EFS_WIRE_RAFT_HDR_LEN;
        for (i = 0; i < msg->nentries; i++)
            cap += 12u + msg->entries[i].clen;
        heap = malloc(cap);
        if (!heap)
            return EFS_ERR_AGAIN;
        buf = heap;
        rc = efs_wire_raft_encode(msg, buf, cap, &len);
    }
    if (rc != EFS_OK) {
        host_wire_taken(msg);
        free(heap);
        return EFS_ERR_AGAIN;
    }
    if (!heap) {
        heap = malloc(len);
        if (!heap)
            return EFS_ERR_AGAIN;
        memcpy(heap, buf, len);
    }
    tx = &h->tx[msg->to];
    pthread_mutex_lock(&h->outbox_mu);
    if (!h->tx_running) {
        tx->st_drop++;
        pthread_mutex_unlock(&h->outbox_mu);
        host_wire_taken(msg);
        free(heap);
        return EFS_ERR_AGAIN;
    }
    if (outbox_is_ae_rep(heap, len) &&
        outbox_pri_replace(tx, outbox_is_ae_rep, heap, len)) {
        tx->st_enq++;
        pthread_cond_signal(&tx->cv);
        pthread_mutex_unlock(&h->outbox_mu);
        host_wire_taken(msg);
        return EFS_OK;
    }
    if (outbox_is_heartbeat(heap, len) &&
        outbox_pri_replace(tx, outbox_is_heartbeat, heap, len)) {
        tx->st_enq++;
        pthread_cond_signal(&tx->cv);
        pthread_mutex_unlock(&h->outbox_mu);
        host_wire_taken(msg);
        return EFS_OK;
    }
    if (tx->npri + tx->nent >= HOST_OUTBOX_MAX) {
        /* A vote takes a heartbeat's slot rather than being dropped.
         * Anything else (an entry AE behind a full lane) is retried
         * by the caller; that is not a drop. */
        int vote = heap[0] == EFS_RAFT_MSG_VOTE_REQ ||
                   heap[0] == EFS_RAFT_MSG_VOTE_REP;
        if (vote && outbox_pri_replace(tx, outbox_is_heartbeat, heap, len)) {
            tx->st_enq++;
            pthread_cond_signal(&tx->cv);
            pthread_mutex_unlock(&h->outbox_mu);
            host_wire_taken(msg);
            return EFS_OK;
        }
        pthread_mutex_unlock(&h->outbox_mu);
        host_wire_taken(msg);
        free(heap);
        return EFS_ERR_AGAIN;
    }
    {
        int entry = outbox_entry_lane(heap, len);
        struct host_outbox_item *slot;
        int *np;

        if (entry) {
            slot = &tx->ent[tx->nent];
            np = &tx->nent;
        } else {
            slot = &tx->pri[tx->npri];
            np = &tx->npri;
        }
        slot->buf = heap;
        slot->len = len;
        (*np)++;
        tx->st_enq++;
        if ((uint32_t)(tx->npri + tx->nent) > tx->st_hi)
            tx->st_hi = (uint32_t)(tx->npri + tx->nent);
        if (!tx->started) {
            if (efsd_pthread_create(&tx->tid, host_sender, tx) == 0)
                tx->started = 1;
            else
                (*np)--; /* no sender: drop rather than queue forever */
        }
    }
    pthread_cond_signal(&tx->cv);
    pthread_mutex_unlock(&h->outbox_mu);
    host_wire_taken(msg);
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
    APPLY_LOG("raft-host: applied mkfs index=%llu salt=%llu\n",
            (unsigned long long)index, (unsigned long long)salt);
    return EFS_OK;
}

/* EFS_MD_CMD_SALT: [anchor:4][salt:8]. Carries the export salt to a group
 * that never applies MKFS (the even-shard group), so every node computes
 * the same efs_kv_mkdir_shard placement. */
static int apply_salt_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                          uint32_t clen, uint64_t index)
{
    uint32_t anchor;
    uint64_t salt;
    int rc;

    if (clen < 13)
        return EFS_OK;
    anchor = rd32be(cmd + 1);
    salt = rd64be(cmd + 5);
    rc = efs_meta_apply_salt_record(h->kv, anchor, salt);
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply salt rc=%d index=%llu\n",
                rc, (unsigned long long)index);
        return rc;
    }
    APPLY_LOG("raft-host: applied salt index=%llu anchor=%u salt=%llu\n",
              (unsigned long long)index, anchor, (unsigned long long)salt);
    return EFS_OK;
}

/* Same encoding as sim pack_create / apply_create_cmd. S_IFDIR is mkdir
 * (same-group fast path). A replay of this command is the opid probe
 * inside the apply (recorded OK). EXIST from the name lookup is a
 * different creator and must stay EXIST: rewriting it to OK made both
 * O_EXCL racers win. host_apply still returns OK to the raft core for
 * CREATE, so the verdict does not stall the log. */
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
    {
        uint32_t tail = (uint32_t)HOST_CREATE_NAME_OFF + nl +
                        EFS_OPID_UUID_LEN + 4;
        uint32_t body = clen;
        struct efs_opid_req q, *qp = NULL;
        efs_ino_t want = 0;
        int lay = -1;

        if ((cmd[1] & HOST_CREATE_F_OPID) && clen >= tail + HOST_OPID_TRAILER) {
            body = clen - HOST_OPID_TRAILER;
            unpack_opid_trailer(cmd + body, &q);
            qp = &q;
        }
        if (body >= tail + 9) {
            want = rd64be(cmd + tail);
            lay = cmd[tail + 8];
        }
        if (S_ISDIR(mode)) {
            if (qp || want)
                rc = efs_meta_apply_mkdir_log_op(h->kv, &at, parent, mode, name,
                                                 want, lay, qp, &ino);
            else
                rc = efs_meta_apply_mkdir(h->kv, &at, parent, mode, name, &ino);
        } else {
            rc = efs_meta_apply_create_file_log_op(h->kv, &at, parent, mode,
                                                   name, want, lay, qp, &ino);
        }
    }
    if (rc == EFS_OK && ino && !S_ISDIR(mode)) {
        const uint8_t *uuid = cmd + HOST_CREATE_NAME_OFF + nl;
        int zi, zero = 1;

        for (zi = 0; zi < EFS_OPID_UUID_LEN; zi++) {
            if (uuid[zi]) {
                zero = 0;
                break;
            }
        }
        if (!zero) {
            struct efs_meta_row row;
            if (efs_meta_apply_get_inode(h->kv, ino, &row) == EFS_OK)
                (void)efs_lease_open(h->kv, ino, row.generation, uuid, 1);
        }
    }
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply create rc=%d index=%llu parent=%llu "
                "name=%s\n",
                rc, (unsigned long long)index, (unsigned long long)parent,
                name);
        return rc;
    }
    APPLY_LOG("raft-host: applied create index=%llu parent=%llu name=%s "
            "ino=%llu\n",
            (unsigned long long)index, (unsigned long long)parent, name,
            (unsigned long long)ino);
    return EFS_OK;
}

/* UNLINK/RMDIR command: base = 18 + nl + uuid/epoch slot; an op-id
 * trailer (I16) is present when the command is exactly that much longer. */
static const struct efs_opid_req *unlink_cmd_opid(const uint8_t *cmd, uint32_t clen,
                                                  uint8_t nl,
                                                  struct efs_opid_req *q)
{
    uint32_t base = 18u + nl + EFS_OPID_UUID_LEN + 4u;

    if (clen < base + HOST_OPID_TRAILER)
        return NULL;
    unpack_opid_trailer(cmd + base, q);
    return q;
}

static int apply_unlink_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                            uint32_t clen, uint64_t index)
{
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint8_t nl;
    uint64_t now;
    struct efs_opid_req q;
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
    rc = efs_meta_apply_unlink_op(h->kv, parent, name, now,
                                  unlink_cmd_opid(cmd, clen, nl, &q));
    /* W45: the rc IS the ring verdict (UNLINK is on host_apply's
     * ring-only list, so it never halts the log). NOT_FOUND here means
     * the dentry went away between the leader's pre-check and the apply
     * (a peer's unlink or rename won) — ENOENT to the caller, which is
     * the POSIX answer. The retry of a committed unlink is answered from
     * the op-id window before the pre-check (I16), not by mapping this to
     * OK; the old "NOT_FOUND → OK (replay)" is the silent mapping W36
     * flagged. BUSY/IO were also OK before — the file stayed and the
     * client heard success. */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply unlink rc=%d index=%llu parent=%llu "
                "name=%s\n",
                rc, (unsigned long long)index, (unsigned long long)parent,
                name);
        return rc;
    }
    APPLY_LOG("raft-host: applied unlink index=%llu parent=%llu name=%s\n",
            (unsigned long long)index, (unsigned long long)parent, name);
    return EFS_OK;
}

static int apply_rmdir_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                           uint32_t clen, uint64_t index)
{
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint8_t nl;
    uint64_t now;
    struct efs_opid_req q;
    int rc;

    if (clen < 18)
        return EFS_ERR_INVAL;
    parent = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    nl = cmd[17];
    if ((uint32_t)18 + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_ERR_INVAL;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + 18, nl);
    rc = efs_meta_apply_rmdir_op(h->kv, parent, name, now,
                                 unlink_cmd_opid(cmd, clen, nl, &q));
    /* W45: NOT_FOUND is the verdict (see apply_unlink_cmd); the retry of a
     * committed rmdir is the op-id window's job. */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply rmdir rc=%d index=%llu parent=%llu "
                "name=%s\n",
                rc, (unsigned long long)index, (unsigned long long)parent,
                name);
        return rc;
    }
    APPLY_LOG("raft-host: applied rmdir index=%llu parent=%llu name=%s\n",
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
    /* W45: the rc is the ring verdict (SETATTR is ring-only in
     * host_apply). NOT_FOUND = the row went away after the leader's
     * pre-read → ENOENT. The handler packs expect_gen = 0, so STALE is
     * not produced; if a caller ever sets it, STALE is a real verdict
     * (the handle names a replaced inode), not a replay. */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply setattr rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return rc;
    }
    APPLY_LOG("raft-host: applied setattr index=%llu ino=%llu mask=%u\n",
            (unsigned long long)index, (unsigned long long)ino, sa.mask);
    return EFS_OK;
}

/* EFS_MD_CMD_XATTR. The verdict (NODATA/EXIST/NOT_FOUND) rides the apply
 * ring; host_apply does not halt the log on it. */
static int apply_xattr_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                           uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    uint64_t now;
    uint32_t flags, vlen, body;
    uint16_t nlen;
    uint8_t op;
    int rc;

    if (clen < HOST_XATTR_HDR)
        return EFS_ERR_INVAL;
    op = cmd[1];
    ino = rd64be(cmd + 2);
    now = rd64be(cmd + 10);
    flags = rd32be(cmd + 18);
    nlen = (uint16_t)(((uint16_t)cmd[22] << 8) | cmd[23]);
    vlen = rd32be(cmd + 24);
    if (nlen > EFS_XATTR_NAME_MAX || vlen > EFS_XATTR_VALUE_MAX)
        return EFS_ERR_INVAL;
    body = (uint32_t)nlen + vlen;
    if (clen != HOST_XATTR_HDR + body)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_xattr(h->kv, ino, now, op, flags, cmd + HOST_XATTR_HDR,
                              nlen, cmd + HOST_XATTR_HDR + nlen, vlen);
    if (rc != EFS_OK && rc != EFS_ERR_NODATA && rc != EFS_ERR_EXIST &&
        rc != EFS_ERR_NOT_FOUND && rc != EFS_ERR_INVAL) {
        fprintf(stderr, "raft-host: apply xattr rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
    }
    return rc;
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
    u.lane_bits = rd64be(cmd + 53);
    rc = efs_meta_apply_utimens(h->kv, ino, now, &u);
    /* W45. STALE is the one documented idempotent case: it means a utimens
     * with a higher mtime_gen already landed (`u->mtime_gen <
     * row.mtime_gen`) — the later caller's stamp is the one POSIX keeps,
     * and this caller's own stat then shows it. NOT_FOUND (row gone after
     * the pre-read) and everything else ride the ring as the verdict
     * (UTIMENS is ring-only in host_apply). The cross-group lane half
     * (lane_bits + expect_gen on a host without the row) returns OK from
     * the apply itself. */
    if (rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply utimens rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return rc;
    }
    APPLY_LOG("raft-host: applied utimens index=%llu ino=%llu mask=%u\n",
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
    t.keep_tail = (cmd[62] & HOST_TRUNC_F_KEEP_TAIL) ? 1 : 0;
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
    /* W43 step b / W45: the apply's rc is the ring verdict (TRUNCATE is
     * ring-only in host_apply, so a failed apply never halts the log).
     * Before, every failure became OK: a lane holding more than the
     * TRUNC_IT_CAP 32 chunk DELs returned NOMEM, the row kept its size,
     * and `truncate -s 0` of a big file reported success (docs/status/README.md
     * W43). Now the SETATTR answers EIO (W16 mapping) until D25 decides
     * the multi-entry shape. NOT_FOUND (row gone after the leader's
     * pre-read) is ENOENT; STALE cannot occur (expect_gen = 0). */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply truncate rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return rc;
    }
    APPLY_LOG("raft-host: applied truncate index=%llu ino=%llu size=%llu\n",
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
    if (clen >= HOST_ACTIVATE_MASK_LEN)
        rc = efs_meta_apply_activate_lanes(h->kv, ino, rd64be(cmd + 9));
    else
        rc = efs_meta_apply_activate_lane(h->kv, ino, cmd[9]);
    /* W45: efs_meta_apply_activate_lanes already answers OK for the two
     * idempotent cases (row unlinked since the host's read; bits already
     * set). Anything else (IO, INVAL) is the verdict the REPORT's
     * host_propose_wait needs — the publish that follows would otherwise
     * land on a lane stat() never collects. Ring-only in host_apply. */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply activate-lane rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return rc;
    }
    if (raft_dbg_on())
        APPLY_LOG("raft-host: applied activate-lane index=%llu ino=%llu\n",
                (unsigned long long)index, (unsigned long long)ino);
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
    /* W43 step b: the fence's rc is the verdict host_truncate waits on.
     * efs_meta_apply_lane_fence is idempotent on its own (a fence at the
     * same or an older epoch returns OK); a NOMEM from the 32-DEL cap or
     * an IO error must stop the truncate before the inode-group entry
     * advances the epoch over chunks that are still there. Ring-only in
     * host_apply. */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply lane-fence rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return rc;
    }
    APPLY_LOG("raft-host: applied lane-fence index=%llu ino=%llu lane=%u\n",
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
        return EFS_ERR_PROTO;
    ino = rd64be(cmd + 1);
    rc = efs_meta_apply_lane_sweep(h->kv, ino, rd64be(cmd + 9), cmd[17]);
    if (rc != EFS_OK)
        fprintf(stderr, "raft-host: apply lane-sweep rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
    return rc;
}

static int apply_orphan_reap_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                  uint32_t len, uint64_t index)
{
    (void)index;
    if (len != 17) return EFS_ERR_PROTO;
    return efs_meta_apply_orphan_reclaim(h->kv, rd64be(cmd + 1), rd64be(cmd + 9));
}

/* EFS_MD_CMD_REAP_DONE on the inode's group: every active lane of the dead
 * inode was swept; clear leftover append state and the reap marker. */
static int apply_reap_done_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                               uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    int rc;

    if (clen < 17)
        return EFS_ERR_PROTO;
    ino = rd64be(cmd + 1);
    rc = efs_meta_apply_reap_done(h->kv, ino, rd64be(cmd + 9));
    if (rc != EFS_OK)
        fprintf(stderr, "raft-host: apply reap-done rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
    return rc;
}

/* EFS_MD_CMD_GC_ACK on the group whose anchor shard holds the records:
 * [cnt:2][(ino:8)(gen:8)(lane:1)(ci:4)(frag:1)]*cnt — one fragment of each
 * record was deleted (or was already gone) on its target node. */
static int apply_gc_ack_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                            uint32_t clen, uint64_t index)
{
    struct efs_gc_ack_item items[EFS_META_GC_ACK_MAX];
    uint32_t cnt, i, off;
    int rc;

    (void)index;
    if (clen < 3)
        return EFS_ERR_PROTO;
    cnt = (uint32_t)((cmd[1] << 8) | cmd[2]);
    if (cnt > EFS_META_GC_ACK_MAX || clen < 3 + cnt * 22)
        return EFS_ERR_PROTO;
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
    return rc;
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
    /* Verdict + reserved eof ride the apply-result ring. Swallowing BUSY
     * as OK made host_wait_applied succeed and the handler reply the live
     * watermark — two clients then both wrote at wm-len. */
    h->apply_extra = (rc == EFS_OK) ? off : 0;
    if (rc != EFS_OK && rc != EFS_ERR_INVAL && rc != EFS_ERR_BUSY) {
        fprintf(stderr, "raft-host: apply append-rsv rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
    } else if (rc == EFS_OK) {
        APPLY_LOG("raft-host: applied append-rsv index=%llu ino=%llu off=%llu\n",
                (unsigned long long)index, (unsigned long long)ino,
                (unsigned long long)off);
    }
    return rc;
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
    APPLY_LOG("raft-host: applied append-res index=%llu ino=%llu off=%llu\n",
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
    APPLY_LOG("raft-host: applied dir index=%llu ino=%llu kind=%u\n",
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
        /* A failed lease close/drop must remain retryable on every replica;
         * advancing past it can pin an unlinked inode forever. */
        return rc == EFS_ERR_IO || rc == EFS_ERR_NOMEM ? rc : EFS_OK;
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
    APPLY_LOG("raft-host: applied lock index=%llu ino=%llu kind=%u\n",
            (unsigned long long)index, (unsigned long long)r.ino,
            (unsigned)cmd[1]);
    return EFS_OK;
}

/* Same encoding as sim apply_publish_cmd (plus the host FileID/lane_local
 * tail). clen may be N * HOST_PUBLISH_LEN — one Raft entry, many pubs. */
static int apply_one_publish(struct efs_raft_host *h, const uint8_t *cmd,
                             uint64_t index)
{
    struct efs_meta_pub p;
    const uint8_t *q;
    int i, rc;

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
    p.delta_off = rd32be(q + 0);
    p.delta_len = rd32be(q + 4);
    p.delta_base_n = rd32be(q + 8);
    p.delta_base_seq = rd64be(q + 12);
    q += EFS_OPID_UUID_LEN + 4;
    p.candidate_gen = rd64be(q);
    p.expected_gen = rd64be(q + 8);
    p.content_epoch = rd64be(q + 16);
    p.coding_profile_id = rd32be(q + 24);
    q += 28;
    p.inode_gen = rd64be(q);
    p.mtime_gen = rd64be(q + 8);
    p.lane_local = (q[16] & HOST_PUB_F_LANE_LOCAL) ? 1 : 0;
    p.fresh_object = !!(q[16] & HOST_PUB_F_FRESH_OBJECT);
    if (cmd[0] == EFS_MD_CMD_PUBLICATION || cmd[0] == EFS_MD_CMD_PUBLICATION_RETIRE) {
        p.durable_result = 1;
        memcpy(p.publication_id.client_uuid,cmd+HOST_PUBLISH_LEN,EFS_OPID_UUID_LEN);
        p.publication_id.session_epoch=rd32be(cmd+HOST_PUBLISH_LEN+16);
        p.publication_id.seq=rd64be(cmd+HOST_PUBLISH_LEN+20);
    }
    rc = cmd[0] == EFS_MD_CMD_PUBLICATION_RETIRE ?
        efs_meta_apply_publication_retire(h->kv, &p) : efs_meta_apply_publish(h->kv, &p);
    /* Deleted inode: P3 no-op. STALE stays on the apply-result ring so
     * host_pub_batch_finish can fail the report (W1 / I12). host_apply
     * must still return OK to the raft core — a losing CAS is a committed
     * log entry, not a reason to pin last_applied. */
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    if (rc != EFS_OK)
        fprintf(stderr,
                "raft-host: apply publish rc=%d index=%llu ino=%llu ci=%u "
                "why=%d span=%u+%u exp=%llx base_n=%u\n",
                rc, (unsigned long long)index, (unsigned long long)p.ino,
                p.chunk_index,
                rc == EFS_ERR_STALE ? efs_meta_apply_publish_stale_why() : 0,
                p.delta_off, p.delta_len,
                (unsigned long long)p.expected_gen, p.delta_base_n);
    else if (raft_dbg_on())
        APPLY_LOG("raft-host: applied publish index=%llu ino=%llu ci=%u\n",
                (unsigned long long)index, (unsigned long long)p.ino,
                p.chunk_index);
    return rc;
}

static int apply_publish_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    uint32_t off = 0, n = 0;
    int held = 0;
    int first = EFS_OK;

    if (clen < HOST_PUBLISH_LEN)
        return EFS_OK;
    /* One WAL fsync for the whole Raft entry. Sequential efs_kv_batch
     * otherwise fsyncs per pub; group-commit only helps concurrent callers. */
    if (clen >= 2u * HOST_PUBLISH_LEN) {
        (void)efs_kv_lsm_sync_hold(h->kv);
        held = 1;
    }

    while (off + HOST_PUBLISH_LEN <= clen) {
        int prc = apply_one_publish(h, cmd + off, index);
        if (first == EFS_OK && prc != EFS_OK)
            first = prc;
        off += HOST_PUBLISH_LEN;
        n++;
    }
    if (held)
        (void)efs_kv_lsm_sync_release(h->kv);
    if (n > 1 && raft_dbg_on())
        APPLY_LOG("raft-host: applied publish-batch index=%llu n=%u\n",
                (unsigned long long)index, n);
    return first;
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
    int rc = EFS_ERR_PROTO, dec, kind;
    uint32_t klen;
    const uint8_t *key;
    struct efs_txn_parts p;

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
        /* Kind-specific payload (EXCL / GUARD / REDUCE / REDUCE_INO /
         * REDUCE_ADD) is decoded in one place for the server and the
         * simulator. */
        rc = efs_txn_apply_prepare(h->kv, kind, &t, &p, key, klen, cmd + off,
                                   clen - off);
        if (rc == EFS_OK && klen >= 2)
            host_rec_mark(h, ((uint32_t)key[0] << 8) | key[1]);
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

/* This replica applied a command whose row is missing here but present on
 * another voter (same last_log, divergent KV). Stay out of leadership so
 * the complete replica can take APPEND/CREATE. */
static void host_mark_incomplete(struct host_group *g)
{
    if (!g || !g->r)
        return;
    if (!g->kv_incomplete) {
        g->kv_incomplete = 1;
        fprintf(stderr,
                "raft-host: group=%u KV incomplete — step down, no campaign\n",
                g->group);
    }
    efs_raft_allow_campaign(g->r, 0);
    efs_raft_step_down(g->r);
}

static void host_clear_incomplete(struct host_group *g)
{
    if (!g || !g->r || !g->kv_incomplete)
        return;
    g->kv_incomplete = 0;
    efs_raft_allow_campaign(g->r, 1);
    fprintf(stderr, "raft-host: group=%u KV complete — campaign allowed\n",
            g->group);
}

static int host_apply(void *app, uint64_t index, uint64_t term,
                      const uint8_t *cmd, uint32_t clen)
{
    struct host_group *g = app;
    struct efs_raft_host *h = g->host;
    uint64_t a0 = 0;
    int rc, ret;

    if (g && __atomic_load_n(&g->import_block, __ATOMIC_ACQUIRE))
        return EFS_ERR_BUSY;
    if (h && __atomic_load_n(&h->snap_batch, __ATOMIC_ACQUIRE))
        return EFS_ERR_BUSY;
    if (!h || !h->kv || !cmd || clen == 0) {
        /* Still stamp the slot: a waiter must never find "never written". */
        g->arc_idx[index & HOST_APPLY_RC_MASK] = index;
        g->arc_term[index & HOST_APPLY_RC_MASK] = term;
        g->arc_rc[index & HOST_APPLY_RC_MASK] = EFS_OK;
        g->arc_extra[index & HOST_APPLY_RC_MASK] = 0;
        return EFS_OK;
    }
    if (raft_obs_on())
        a0 = now_us_();
    h->apply_extra = 0;
    rc = host_apply_dispatch(h, index, cmd, clen);
    /* A successful CREATE/APPEND means this replica's KV has the row.
     * Re-enable campaign so one hollow apply cannot leave the group with
     * zero candidates. */
    if (rc == EFS_OK &&
        (cmd[0] == EFS_MD_CMD_CREATE || cmd[0] == EFS_MD_CMD_APPEND_RSV))
        host_clear_incomplete(g);
    /* Only the LEADER steps down on a hollow APPEND apply. Followers that
     * miss a row must stay eligible — marking every voter was how g2 lost
     * all campaigners and APPEND timed out NET. CREATE NOT_FOUND is not
     * hollow-leader (parent on the other group / dual-host LSM). */
    if (rc == EFS_ERR_NOT_FOUND && cmd[0] == EFS_MD_CMD_APPEND_RSV &&
        g->r && efs_raft_role(g->r) == EFS_RAFT_LEADER)
        host_mark_incomplete(g);
    /* Record the apply verdict BEFORE the raft core bumps last_applied, so a
     * waiter that observes applied >= index always finds the slot. The pump
     * holds h->mu across apply_committed, and host_propose_wait reads the
     * ring under h->mu, so this is race-free. */
    g->arc_idx[index & HOST_APPLY_RC_MASK] = index;
    g->arc_term[index & HOST_APPLY_RC_MASK] = term;
    g->arc_rc[index & HOST_APPLY_RC_MASK] = rc;
    g->arc_extra[index & HOST_APPLY_RC_MASK] = h->apply_extra;
    /* Txn apply never stalls the log: a conflict verdict rides the ring to
     * the proposer, it is not a raft-core error. Other cmds keep their
     * historic return (halt-on-error for the few that can fail).
     * APPEND_RSV is the same: BUSY/INVAL must not halt the log.
     * PUBLISH too: a lost generation CAS is audible via the ring, not a
     * reason to retry the same index forever (W1 two-client RMW). */
    /* SALT too (W37): a SALT that does not match the anchor's record
     * answers PROTO and writes nothing; it must not halt the group's
     * apply (Oct 1: `apply salt rc=-7 index=3` on every group-2 apply).
     * W43/W45 (Oct 1): UNLINK, SETATTR, UTIMENS, TRUNCATE, ACTIVATE_LANE
     * and LANE_FENCE now return their apply rc as the verdict instead of
     * swallowing it as OK; a non-OK verdict is a reply to the proposer,
     * never a reason to re-apply the same index. */
    ret = (cmd[0] == EFS_MD_CMD_PREPARE || cmd[0] == EFS_MD_CMD_DECIDE ||
           cmd[0] == EFS_MD_CMD_RESOLVE || cmd[0] == EFS_MD_CMD_DROP ||
           cmd[0] == EFS_MD_CMD_RMDIR || cmd[0] == EFS_MD_CMD_APPEND_RSV ||
           cmd[0] == EFS_MD_CMD_CREATE || cmd[0] == EFS_MD_CMD_PUBLISH ||
           cmd[0] == EFS_MD_CMD_PUBLICATION || cmd[0] == EFS_MD_CMD_PUBLICATION_RETIRE ||
           cmd[0] == EFS_MD_CMD_XATTR || cmd[0] == EFS_MD_CMD_SALT ||
           cmd[0] == EFS_MD_CMD_UNLINK || cmd[0] == EFS_MD_CMD_SETATTR ||
           cmd[0] == EFS_MD_CMD_UTIMENS || cmd[0] == EFS_MD_CMD_TRUNCATE ||
           cmd[0] == EFS_MD_CMD_ACTIVATE_LANE ||
           cmd[0] == EFS_MD_CMD_LANE_FENCE ||
           cmd[0] == EFS_MD_CMD_LANE_SWEEP || cmd[0] == EFS_MD_CMD_REAP_DONE ||
           cmd[0] == EFS_MD_CMD_GC_ACK || cmd[0] == EFS_MD_CMD_ORPHAN_REAP)
              ? EFS_OK
              : rc;
    /* Storage failure must retry this entry on every replica, rather than
     * advance a follower past cleanup it did not durably perform. */
    if ((cmd[0] == EFS_MD_CMD_LANE_SWEEP || cmd[0] == EFS_MD_CMD_REAP_DONE ||
         cmd[0] == EFS_MD_CMD_GC_ACK || cmd[0] == EFS_MD_CMD_ORPHAN_REAP) &&
        (rc == EFS_ERR_IO || rc == EFS_ERR_NOMEM))
        ret = rc;
    if (a0) {
        h->obs_apply_cnt++;
        h->obs_apply_cycle_us += now_us_() - a0;
    }
    return ret;
}

/* Snapshot point = flush + save_snap. The bytes are a pinned view written
 * on the GC thread to mdraft/snap-<group>-<incl>.kvx; send_snap preads
 * that file. A BUSY open means the file is not ready yet. */
static int host_snap_open(void *app, uint64_t incl, void **handle,
                          uint64_t *total)
{
    struct host_group *g = app;
    struct efs_raft_host *h = g->host;
    struct efs_kv_lsm_view *view = NULL;
    int fd, rc;

    if (!h || !h->kv || !handle || !total)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&h->snap_mu);
    if (g->snap_ready && g->snap_incl == incl && g->snap_fd >= 0) {
        fd = dup(g->snap_fd);
        *total = g->snap_bytes;
        pthread_mutex_unlock(&h->snap_mu);
        if (fd < 0)
            return EFS_ERR_IO;
        *handle = (void *)(intptr_t)fd;
        return EFS_OK;
    }
    if (g->snap_exporting) {
        pthread_mutex_unlock(&h->snap_mu);
        return EFS_ERR_BUSY;
    }
    if (!g->r || incl != efs_raft_applied(g->r)) {
        pthread_mutex_unlock(&h->snap_mu);
        return EFS_ERR_NOT_FOUND;
    }
    pthread_mutex_unlock(&h->snap_mu);
    /* The pump calls this on every catch-up heartbeat. A flush that
     * would walk the memtable and return BUSY (L0 cannot take another
     * full set of ranges) stays off this thread. */
    rc = efs_kv_lsm_flush_nowait(h->kv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_lsm_view_pin(h->kv, &view);
    if (rc != EFS_OK)
        return rc;
    pthread_mutex_lock(&h->snap_mu);
    if (g->snap_exporting) {
        pthread_mutex_unlock(&h->snap_mu);
        efs_kv_lsm_view_unpin(view);
        return EFS_ERR_BUSY;
    }
    g->snap_view = view;
    g->snap_incl = incl;
    g->snap_exporting = 1;
    g->snap_ready = 0;
    snprintf(g->snap_path, sizeof(g->snap_path), "%s/snap-%u-%llu.kvx",
             h->mdraft, g->group, (unsigned long long)incl);
    pthread_mutex_unlock(&h->snap_mu);
    return EFS_ERR_BUSY;
}

static int host_snap_read(void *handle, uint64_t offset, uint8_t *buf,
                          uint32_t n, uint32_t *got)
{
    int fd = (int)(intptr_t)handle;
    ssize_t r;

    if (fd < 0 || !buf || !got)
        return EFS_ERR_INVAL;
    r = pread(fd, buf, n, (off_t)offset);
    if (r < 0)
        return EFS_ERR_IO;
    *got = (uint32_t)r;
    return EFS_OK;
}

static void host_snap_close(void *handle)
{
    int fd = (int)(intptr_t)handle;

    if (fd >= 0)
        close(fd);
}

/* import_state. snap_mu. */
#define SNAP_IMP_IDLE   0
#define SNAP_IMP_QUEUED 1
#define SNAP_IMP_RUN    2
#define SNAP_IMP_DONE   3
#define SNAP_IMP_FAIL   4
#define SNAP_IMP_APPLY  5
/* Keys of an import diff the pump writes per cycle, then returns so
 * the same cycle can answer AppendEntries. A fixed count, not a knob:
 * one tick is 5 ms and a point put is well under that at this size. */
#define HOST_SNAP_SLICE 1024

static void snap_import_free_diff(struct host_group *g)
{
    free(g->import_diff);
    free(g->import_hold_a);
    free(g->import_hold_b);
    g->import_diff = NULL;
    g->import_hold_a = NULL;
    g->import_hold_b = NULL;
    g->import_ni = 0;
    g->import_at = 0;
}

/* Drop a queued import, or invalidate one the GC thread is already
 * scanning. A prepared diff is discarded here, not written: the pump
 * is the only thread that batches it, in the ack that installs the snap.
 * A cancel of RUN leaves import_block for the GC thread. */
static void snap_import_cancel(struct efs_raft_host *h, struct host_group *g)
{
    int st;

    pthread_mutex_lock(&h->snap_mu);
    st = g->import_state;
    g->import_gen++;
    g->import_state = SNAP_IMP_IDLE;
    /* A cancelled import may have applied slices already: the watermark
     * no longer tracks this anchor's GC records (D26). */
    efs_meta_gc_pending_invalidate(g->group == 0 ? 1u : 2u);
    /* RUN: the GC thread owns the buffers. APPLY: the pump is mid-slice
     * and frees them when it notices the state left APPLY. */
    if (st != SNAP_IMP_RUN && !g->import_applying)
        snap_import_free_diff(g);
    if (st == SNAP_IMP_QUEUED || st == SNAP_IMP_DONE || st == SNAP_IMP_FAIL)
        __atomic_store_n(&g->import_block, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&h->snap_mu);
}

/* 1 if this incl already has an import queued, running, or finished.
 * *rc is BUSY, OK, or the import error. */
static int snap_import_poll(struct efs_raft_host *h, struct host_group *g,
                            uint64_t incl, uint64_t term, int *rc)
{
    int st;

    pthread_mutex_lock(&h->snap_mu);
    if (g->import_incl != incl || g->import_term != term ||
        g->import_state == SNAP_IMP_IDLE) {
        pthread_mutex_unlock(&h->snap_mu);
        return 0;
    }
    st = g->import_state;
    if (st == SNAP_IMP_QUEUED || st == SNAP_IMP_RUN) {
        pthread_mutex_unlock(&h->snap_mu);
        *rc = EFS_ERR_BUSY;
        return 1;
    }
    if (st == SNAP_IMP_DONE) {
        struct efs_kv_item *diff = g->import_diff;
        uint32_t at = g->import_at;
        uint32_t ni = g->import_ni;
        uint32_t n;

        n = (at < ni) ? ni - at : 0;
        if (n > HOST_SNAP_SLICE)
            n = HOST_SNAP_SLICE;
        g->import_applying = 1;
        g->import_state = SNAP_IMP_APPLY;
        pthread_mutex_unlock(&h->snap_mu);
        /* One slice, then back to the pump so heartbeats go out before
         * the leader retries this chunk. The ack (OK) waits until the
         * cursor reaches ni; until then the retry gets BUSY. */
        __atomic_store_n(&h->snap_batch, 1, __ATOMIC_RELEASE);
        *rc = n ? efs_kv_batch(h->kv, diff + at, n) : EFS_OK;
        __atomic_store_n(&h->snap_batch, 0, __ATOMIC_RELEASE);
        pthread_mutex_lock(&h->snap_mu);
        g->import_applying = 0;
        if (g->import_state != SNAP_IMP_APPLY) {
            snap_import_free_diff(g);
            __atomic_store_n(&g->import_block, 0, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&h->snap_mu);
            *rc = EFS_ERR_BUSY;
            return 1;
        }
        if (*rc != EFS_OK) {
            g->import_state = SNAP_IMP_DONE;
            pthread_mutex_unlock(&h->snap_mu);
            return 1;
        }
        g->import_at = at + n;
        if (g->import_at < g->import_ni) {
            g->import_state = SNAP_IMP_DONE;
            pthread_mutex_unlock(&h->snap_mu);
            *rc = EFS_ERR_BUSY;
            return 1;
        }
        g->part_off = 0;
        g->import_state = SNAP_IMP_IDLE;
        snap_import_free_diff(g);
        /* The diff replaced this group's keyspace — the pending-GC
         * watermark is re-derived on the next pass (D26). */
        efs_meta_gc_pending_invalidate(g->group == 0 ? 1u : 2u);
        pthread_mutex_unlock(&h->snap_mu);
        unlink(g->part_path);
        __atomic_store_n(&g->import_block, 0, __ATOMIC_RELEASE);
        *rc = EFS_OK;
        return 1;
    }
    *rc = g->import_rc ? g->import_rc : EFS_ERR_IO;
    g->import_state = SNAP_IMP_IDLE;
    efs_meta_gc_pending_invalidate(g->group == 0 ? 1u : 2u);
    __atomic_store_n(&g->import_block, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&h->snap_mu);
    return 1;
}

static int host_snap_chunk(void *app, uint64_t incl, uint64_t incl_term,
                           uint64_t offset, const uint8_t *data, uint32_t len,
                           int done)
{
    struct host_group *g = app;
    struct efs_raft_host *h = g->host;
    int rc;

    if (!h || !h->kv)
        return EFS_ERR_INVAL;
    /* Retry of the chunk that closed the file, including a one-chunk
     * snapshot whose retry comes back at offset 0. The bytes are already
     * in the .part; the GC thread is diffing them into the KV. */
    if (done && snap_import_poll(h, g, incl, incl_term, &rc))
        return rc;
    if (offset == 0) {
        snap_import_cancel(h, g);
        if (g->part_fd >= 0) {
            close(g->part_fd);
            unlink(g->part_path);
            g->part_fd = -1;
        }
        snprintf(g->part_path, sizeof(g->part_path),
                 "%s/snap-%u-%llu.part", h->mdraft, g->group,
                 (unsigned long long)incl);
        g->part_fd = open(g->part_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (g->part_fd < 0)
            return EFS_ERR_IO;
        g->part_incl = incl;
        g->part_term = incl_term;
        g->part_off = 0;
    } else if (incl != g->part_incl || incl_term != g->part_term ||
               offset != g->part_off || g->part_fd < 0) {
        return EFS_ERR_INVAL;
    }
    if (len) {
        uint32_t left = len;
        const uint8_t *p = data;

        if (!p)
            return EFS_ERR_INVAL;
        while (left) {
            ssize_t w = pwrite(g->part_fd, p, left, (off_t)g->part_off);

            if (w <= 0)
                return EFS_ERR_IO;
            g->part_off += (uint64_t)w;
            p += w;
            left -= (uint32_t)w;
        }
    }
    if (!done)
        return EFS_OK;
    if (fsync(g->part_fd) != 0)
        return EFS_ERR_IO;
    close(g->part_fd);
    g->part_fd = -1;
    /* Scan and sort on the GC thread. This returns before the diff is
     * applied; the leader retries this chunk until that finishes. */
    pthread_mutex_lock(&h->snap_mu);
    g->import_gen++;
    g->import_incl = incl;
    g->import_term = incl_term;
    snprintf(g->import_path, sizeof(g->import_path), "%s", g->part_path);
    g->import_state = SNAP_IMP_QUEUED;
    pthread_mutex_unlock(&h->snap_mu);
    return EFS_ERR_BUSY;
}

static int host_maybe_snapshot(struct efs_raft_host *h, int gi)
{
    uint64_t applied, snap;
    int exporting;

    if (!h->g[gi].hosted || !h->g[gi].r)
        return EFS_OK;
    pthread_mutex_lock(&h->snap_mu);
    exporting = h->g[gi].snap_exporting;
    pthread_mutex_unlock(&h->snap_mu);
    if (exporting)
        return EFS_OK;
    applied = efs_raft_applied(h->g[gi].r);
    snap = efs_raft_snap_index(h->g[gi].r);
    /* Byte budget, not an entry count. HOST_SNAP_MIN (256) exported the
     * whole KV every few seconds. A store that reports log bytes
     * snapshots once per EFS_RAFT_SNAP_BYTES of commands past snap_idx
     * and keeps that window for AppendEntries. */
    if (efs_raft_tracks_log_bytes(h->g[gi].r)) {
        if (efs_raft_log_new_bytes(h->g[gi].r) < EFS_RAFT_SNAP_BYTES)
            return EFS_OK;
    } else if (applied < snap + HOST_SNAP_MIN) {
        return EFS_OK;
    }
    /* efs_raft_snapshot → host_snap_open flushes again and pins. A BUSY
     * open still records the snap point; the file follows on the GC thread. */
    return efs_raft_snapshot(h->g[gi].r);
}

static int host_apply_dispatch(struct efs_raft_host *h, uint64_t index,
                               const uint8_t *cmd, uint32_t clen)
{
    if (cmd[0] == EFS_MD_CMD_MKFS)
        return apply_mkfs_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_SALT)
        return apply_salt_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_CREATE)
        return apply_create_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_UNLINK)
        return apply_unlink_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_RMDIR)
        return apply_rmdir_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_PUBLICATION || cmd[0] == EFS_MD_CMD_PUBLICATION_RETIRE)
        return clen == HOST_PUBLICATION_LEN ? apply_one_publish(h,cmd,index) : EFS_ERR_PROTO;
    if (cmd[0] == EFS_MD_CMD_PUBLISH)
        return apply_publish_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_SETATTR)
        return apply_setattr_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_XATTR)
        return apply_xattr_cmd(h, cmd, clen, index);
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
    if (cmd[0] == EFS_MD_CMD_ORPHAN_REAP)
        return apply_orphan_reap_cmd(h, cmd, clen, index);
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

#define V_LOAD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define V_STORE(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* Publish one group's replica state for lock-free readers. h->mu held. */
static void host_publish_view(struct efs_raft_host *h, int gi)
{
    struct host_group *g = &h->g[gi];
    struct efs_raft *r = g->r;

    uint64_t commit, applied, term, ridx;
    int role, leader, rpend, rdone, changed;

    if (!r) {
        if (V_LOAD(g->v_has))
            V_STORE(g->v_stamp, V_LOAD(g->v_stamp) + 1);
        V_STORE(g->v_has, 0);
        return;
    }
    commit = efs_raft_commit(r);
    applied = efs_raft_applied(r);
    term = efs_raft_term(r);
    role = (int)efs_raft_role(r);
    leader = efs_raft_leader(r);
    ridx = efs_raft_read_index(r);
    rpend = efs_raft_read_pending(r);
    rdone = efs_raft_read_done(r);
    changed = !V_LOAD(g->v_has) || V_LOAD(g->v_commit) != commit ||
              V_LOAD(g->v_applied) != applied || V_LOAD(g->v_term) != term ||
              V_LOAD(g->v_role) != role || V_LOAD(g->v_leader) != leader ||
              V_LOAD(g->v_read_index) != ridx ||
              V_LOAD(g->v_read_pending) != rpend ||
              V_LOAD(g->v_read_done) != rdone;
    V_STORE(g->v_commit, commit);
    V_STORE(g->v_applied, applied);
    V_STORE(g->v_term, term);
    V_STORE(g->v_role, role);
    V_STORE(g->v_leader, leader);
    V_STORE(g->v_read_index, ridx);
    V_STORE(g->v_read_pending, rpend);
    V_STORE(g->v_read_done, rdone);
    V_STORE(g->v_has, 1);
    /* Stamp last: a waiter that read the new stamp has the new fields. */
    if (changed)
        V_STORE(g->v_stamp, V_LOAD(g->v_stamp) + 1);
}

static void host_publish_group(struct efs_raft_host *h, uint8_t group)
{
    int i;
    for (i = 0; i < HOST_NGROUPS; i++)
        if (h->g[i].group == group && h->g[i].hosted)
            host_publish_view(h, i);
}

/* Snapshot of the view (consistent enough for a predicate check: every
 * field is monotone or re-validated by the caller's loop). */
struct host_view {
    int has, role, leader, read_pending, read_done;
    uint64_t commit, applied, read_index;
    uint64_t stamp; /* read FIRST: the fields are from this publish or later */
};

static void host_view_get(struct efs_raft_host *h, uint8_t group,
                          struct host_view *v)
{
    struct host_group *g = group_slot(h, group);

    memset(v, 0, sizeof(*v));
    v->leader = -1;
    if (!g)
        return;
    v->stamp = V_LOAD(g->v_stamp);
    if (!V_LOAD(g->v_has))
        return;
    v->has = 1;
    v->commit = V_LOAD(g->v_commit);
    v->applied = V_LOAD(g->v_applied);
    v->read_index = V_LOAD(g->v_read_index);
    v->role = V_LOAD(g->v_role);
    v->leader = V_LOAD(g->v_leader);
    v->read_pending = V_LOAD(g->v_read_pending);
    v->read_done = V_LOAD(g->v_read_done);
}

static int host_view_covers(const struct host_view *v, uint64_t want)
{
    return v->has && v->role == EFS_RAFT_LEADER && v->read_done &&
           v->read_index >= want && v->applied >= v->read_index;
}

/* Sleep until the pump wakes this waiter or `end` passes. cv_mu HELD by
 * the caller, who has just re-read the view under it and found its
 * predicate false (that order is what rules out a lost wakeup). any=0:
 * wake when the group's applied >= idx (or the replica goes away); any=1:
 * wake when the group's view stamp moves past `seen`. Falls back to the
 * shared applied_cv when all slots are busy. Returns with cv_mu held. */
static void host_waiter_sleep(struct efs_raft_host *h, uint8_t group, int any,
                              uint64_t idx, uint64_t seen,
                              const struct timespec *end)
{
    uint32_t i, n = HOST_WAITERS;
    struct host_waiter *w = NULL;

    for (i = 0; i < n; i++) {
        uint32_t k = (h->waiter_hint + i) % n;
        if (!h->waiters[k].active) {
            w = &h->waiters[k];
            h->waiter_hint = k + 1;
            break;
        }
    }
    if (!w) {
        h->cv_overflow++;
        pthread_cond_timedwait(&h->applied_cv, &h->cv_mu, end);
        h->cv_overflow--;
        return;
    }
    w->group = group;
    w->any = (uint8_t)any;
    w->idx = idx;
    w->seen = seen;
    w->active = 1;
    pthread_cond_timedwait(&w->cv, &h->cv_mu, end);
    w->active = 0;
}

/* Pump side: after publishing every hosted group's view, wake the waiters
 * whose predicate that view satisfies, and only those. */
static void host_waiters_wake(struct efs_raft_host *h)
{
    uint32_t k;

    pthread_mutex_lock(&h->cv_mu);
    for (k = 0; k < HOST_WAITERS; k++) {
        struct host_waiter *w = &h->waiters[k];
        struct host_group *g;
        int wake;

        if (!w->active)
            continue;
        g = group_slot(h, w->group);
        if (!g || !V_LOAD(g->v_has))
            wake = 1;
        else if (w->any)
            wake = V_LOAD(g->v_stamp) != w->seen;
        else
            wake = V_LOAD(g->v_applied) >= w->idx;
        if (wake)
            pthread_cond_signal(&w->cv);
    }
    if (h->cv_overflow)
        pthread_cond_broadcast(&h->applied_cv);
    pthread_mutex_unlock(&h->cv_mu);
}

static int host_hosts(struct efs_raft_host *h, uint8_t group)
{
    struct host_group *s = group_slot(h, group);

    /* A learner replica exists so it can catch up; it must not serve
     * inode RPCs until COLD puts this id in app_old. */
    if (!s || !s->r)
        return 0;
    return hosts_group(h->raft_id, efs_raft_voters(s->r));
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
/* Leader of a group this host does not hold a view of, as the last
 * forwarded reply named it (raft id, -1 unknown). Indexed by group >> 1. */
static int g_fwd_leader[HOST_NGROUPS] = {-1, -1};

static int host_local_leader(struct efs_raft_host *h, uint8_t group);

/* A single-group forward goes to that group's leader when one is known:
 * a follower answers a read with a ReadIndex RPC to the leader plus a
 * catch-up wait (700 us each on the Spark pair, two per cross-group
 * LOOKUP), the leader answers from its covered view in ~20 us. */
static int host_fwd_leader_pick(struct efs_raft_host *h, uint8_t group,
                                int skip)
{
    int rid = -1;

    if ((group >> 1) >= HOST_NGROUPS)
        return -1;
    if (host_hosts(h, group))
        rid = host_local_leader(h, group);
    if (rid < 0)
        rid = __atomic_load_n(&g_fwd_leader[group >> 1], __ATOMIC_RELAXED);
    if (rid < 0 || rid >= h->n || rid == h->raft_id || rid == skip)
        return -1;
    {
        struct host_group *s = group_slot(h, group);
        uint32_t voters = (s && s->r) ? efs_raft_voters(s->r)
                          : (s ? s->voters : group_voters(group, h->n));
        if (!hosts_group(rid, voters))
            return -1;
    }
    return rid;
}

static void host_fwd_leader_note(uint8_t group, int rid)
{
    if ((group >> 1) >= HOST_NGROUPS || rid < 0)
        return;
    __atomic_store_n(&g_fwd_leader[group >> 1], rid, __ATOMIC_RELAXED);
}

static int host_pick_peer(struct efs_raft_host *h, const uint8_t *groups, int ng,
                          int skip)
{
    int rid, i;

    if (ng == 1) {
        rid = host_fwd_leader_pick(h, groups[0], skip);
        if (rid >= 0)
            return rid;
    }
    for (rid = 0; rid < h->n; rid++) {
        int ok = 1;
        if (rid == h->raft_id || rid == skip)
            continue;
        for (i = 0; i < ng; i++) {
            struct host_group *s = group_slot(h, groups[i]);
            uint32_t voters = (s && s->r) ? efs_raft_voters(s->r)
                              : (s ? s->voters
                                 : group_voters(groups[i], h->n));
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
                           struct efs_msg_raft_mkfs_reply *rep, int io_ms)
{
    uint8_t stack[1 + HOST_CMD_MAX];
    uint8_t *payload = stack;
    uint32_t plen;
    char host[64];
    uint16_t port = 0;
    struct efs_conn *pc;
    uint8_t rtype = 0;
    void *reply = NULL;
    uint32_t rlen = 0;
    int rc = EFS_OK;

    memset(rep, 0, sizeof(*rep));
    /* HOST_CMD_MAX is the stack buffer, not a protocol max. A publication
     * batch is HOST_PUB_BATCH_N × HOST_PUBLISH_LEN; rejecting it at 512
     * made every g2-follower forward return INVAL, and host_remote_cmd
     * remapped that to NOT_PRIMARY (fio end_fsync -15). */
    if (rid < 0 || rid == h->raft_id || clen > EFS_WIRE_RAFT_MAX_CMD)
        return EFS_ERR_INVAL;
    plen = 1u + clen;
    if (clen > HOST_CMD_MAX) {
        payload = malloc(plen);
        if (!payload)
            return EFS_ERR_NOMEM;
    }
    payload[0] = group;
    if (clen)
        memcpy(payload + 1, cmd, clen);
    if (peer_addr(h, rid, host, sizeof(host), &port) != 0) {
        rc = EFS_ERR_NOT_PRIMARY;
        goto out;
    }
    pc = server_peer_conn_get(host, port);
    if (!pc) {
        rc = EFS_ERR_BUSY;
        goto out;
    }
    if (pc->kind == EFS_CONN_TCP && pc->fd >= 0 && io_ms > 0) {
        efs_set_recv_timeout(pc->fd, io_ms);
        efs_set_send_timeout(pc->fd, io_ms);
    }
    if (efs_conn_send_msg(pc, EFS_MSG_RAFT_MKFS, payload, plen) != 0) {
        server_peer_conn_drop(host, port, pc);
        rc = EFS_ERR_IO;
        goto out;
    }
    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0 ||
        rtype != EFS_MSG_RAFT_MKFS_REPLY || rlen < sizeof(*rep)) {
        free(reply);
        server_peer_conn_drop(host, port, pc);
        rc = EFS_ERR_IO;
        goto out;
    }
    memcpy(rep, reply, sizeof(*rep));
    free(reply);
    if (pc->kind == EFS_CONN_TCP && pc->fd >= 0) {
        efs_set_recv_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
    }
    server_peer_conn_release(host, port, pc);
out:
    if (payload != stack)
        free(payload);
    return rc;
}

/* Submit cmd (or ReadIndex if clen==0) to the group's leader. Never targets
 * self. prefer_rid is a hint; NOT_PRIMARY replies retry leader_hint. */
static void host_deadline_us(struct timespec *ts, long us);
static int host_past_deadline(const struct timespec *end);

/* The local replica's view of the group's leader, or -1 (lock-free). */
static int host_local_leader(struct efs_raft_host *h, uint8_t group)
{
    struct host_view v;

    host_view_get(h, group, &v);
    return v.has ? v.leader : -1;
}

static int host_remote_cmd(struct efs_raft_host *h, uint8_t group,
                           const uint8_t *cmd, uint32_t clen,
                           struct efs_msg_raft_mkfs_reply *rep, int prefer_rid)
{
    struct timespec end;
    int rid, skip = -1, rc;

    /* A peer that is not the leader answers NOT_PRIMARY (+hint) instead
     * of forwarding on (server_raft_host_submit). During an election
     * nobody has a hint; ride that out here, within the read budget,
     * rather than failing the client's op or parking on a chain of
     * forwards. Each retry re-reads the local replica's leader view. */
    host_deadline_us(&end, (long)HOST_READ_TRIES * HOST_TICK_US);
    for (;;) {
        rid = prefer_rid;
        if (rid < 0 || rid == h->raft_id || rid == skip)
            rid = host_local_leader(h, group);
        if (rid < 0 || rid == h->raft_id || rid == skip)
            rid = host_pick_peer(h, &group, 1, skip);
        if (rid < 0 || rid == h->raft_id) {
            if (host_past_deadline(&end))
                return EFS_ERR_NOT_PRIMARY;
            usleep(HOST_TICK_US);
            skip = -1;
            prefer_rid = -1;
            continue;
        }
        rc = host_rpc_submit(h, rid, group, cmd, clen, rep, EFS_IO_TIMEOUT_MS);
        if (rc != EFS_OK) {
            if (host_past_deadline(&end))
                return rc;
            usleep(HOST_TICK_US);
            skip = rid;
            prefer_rid = -1;
            continue;
        }
        if (rep->rc == EFS_OK)
            return EFS_OK;
        if (rep->rc != EFS_ERR_NOT_PRIMARY)
            return rep->rc;
        if (host_past_deadline(&end))
            return EFS_ERR_NOT_PRIMARY;
        if (rep->leader_hint >= 0 && rep->leader_hint != h->raft_id &&
            rep->leader_hint != rid) {
            prefer_rid = rep->leader_hint;
            skip = rid;
            continue;
        }
        /* No leader known anywhere yet: wait a tick, then re-pick. */
        usleep(HOST_TICK_US);
        skip = rid;
        prefer_rid = -1;
    }
}

static int host_wait_applied(struct efs_raft_host *h, uint8_t group,
                             uint64_t idx, int *leader_hint);
static void host_pump_kick(struct efs_raft_host *h);

/* Threads inside host_propose, including those blocked on h->mu.
 * The pump does not send while this is non-zero, so the send sees
 * every append that was already queued instead of the one that
 * happened to hold the lock. */
static unsigned propose_q;

static void propose_q_add(void)
{
    __atomic_add_fetch(&propose_q, 1u, __ATOMIC_RELEASE);
}

static void propose_q_sub(void)
{
    __atomic_sub_fetch(&propose_q, 1u, __ATOMIC_ACQ_REL);
}

static int propose_q_busy(void)
{
    return __atomic_load_n(&propose_q, __ATOMIC_ACQUIRE) != 0;
}

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

/* EFS_READ_PROF=1: where a LOOKUP / GETATTR spends its time on this
 * host. The Spark du (Oct 2) paid 1.4 ms per stat with the client and
 * the leader on one host and ~100 us of CPU in it; the rest is the
 * ReadIndex round (leader: begin round, pump kick, heartbeat RTT,
 * publish, waiter wake; follower: RPC to the leader + catch-up) and
 * the KV gets. One line per 1000 lookups+getattrs on stderr; sums are
 * microseconds. Counters are relaxed atomics, the hot path adds a few
 * clock reads only when the env is set. */
struct read_prof_acc {
    uint64_t n, sum, max;
};

static struct {
    struct read_prof_acc ri_leader;   /* host_read_index, this host leads */
    struct read_prof_acc ri_leader_covered; /* ... and no new round needed */
    struct read_prof_acc ri_remote;   /* host_read_index, forwarded */
    struct read_prof_acc ri_remote_wait; /* host_wait_applied after it */
    struct read_prof_acc lk_total, lk_ri_parent, lk_kv, lk_lanes, lk_getattr;
    struct read_prof_acc ga_total, ga_lanes, ga_getattr;
    uint64_t lk_fwd, ga_fwd;
    uint64_t ticks;
} g_read_prof;

static int read_prof_on(void)
{
    static int v = -1;
    if (v < 0)
        v = env_on("EFS_READ_PROF");
    return v;
}

static void read_prof_add(struct read_prof_acc *a, uint64_t us)
{
    uint64_t m;

    __atomic_fetch_add(&a->n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&a->sum, us, __ATOMIC_RELAXED);
    m = __atomic_load_n(&a->max, __ATOMIC_RELAXED);
    while (us > m &&
           !__atomic_compare_exchange_n(&a->max, &m, us, 1, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        ;
}

static void read_prof_fmt(char *buf, size_t cap, const char *name,
                          const struct read_prof_acc *a)
{
    uint64_t n = __atomic_load_n(&a->n, __ATOMIC_RELAXED);
    uint64_t s = __atomic_load_n(&a->sum, __ATOMIC_RELAXED);
    uint64_t m = __atomic_load_n(&a->max, __ATOMIC_RELAXED);

    snprintf(buf, cap, " %s=%llu/%llu/%llu", name, (unsigned long long)n,
             (unsigned long long)(n ? s / n : 0), (unsigned long long)m);
}

/* Called once per finished LOOKUP / GETATTR; prints every 1000. Fields
 * are n/avg_us/max_us since start. */
static void read_prof_tick(void)
{
    uint64_t t = __atomic_add_fetch(&g_read_prof.ticks, 1, __ATOMIC_RELAXED);
    char line[1024];
    char f[16][96];
    size_t i;

    if (t % 1000 != 0)
        return;
    read_prof_fmt(f[0], sizeof(f[0]), "lk", &g_read_prof.lk_total);
    read_prof_fmt(f[1], sizeof(f[1]), "lk_ri_parent", &g_read_prof.lk_ri_parent);
    read_prof_fmt(f[2], sizeof(f[2]), "lk_kv", &g_read_prof.lk_kv);
    read_prof_fmt(f[3], sizeof(f[3]), "lk_lanes", &g_read_prof.lk_lanes);
    read_prof_fmt(f[4], sizeof(f[4]), "lk_getattr", &g_read_prof.lk_getattr);
    read_prof_fmt(f[5], sizeof(f[5]), "ga", &g_read_prof.ga_total);
    read_prof_fmt(f[6], sizeof(f[6]), "ga_lanes", &g_read_prof.ga_lanes);
    read_prof_fmt(f[7], sizeof(f[7]), "ga_getattr", &g_read_prof.ga_getattr);
    read_prof_fmt(f[8], sizeof(f[8]), "ri_leader", &g_read_prof.ri_leader);
    read_prof_fmt(f[9], sizeof(f[9]), "ri_leader_covered",
                  &g_read_prof.ri_leader_covered);
    read_prof_fmt(f[10], sizeof(f[10]), "ri_remote", &g_read_prof.ri_remote);
    read_prof_fmt(f[11], sizeof(f[11]), "ri_remote_wait",
                  &g_read_prof.ri_remote_wait);
    line[0] = '\0';
    for (i = 0; i < 12; i++)
        strncat(line, f[i], sizeof(line) - strlen(line) - 1);
    fprintf(stderr, "read-prof:%s lk_fwd=%llu ga_fwd=%llu\n", line,
            (unsigned long long)__atomic_load_n(&g_read_prof.lk_fwd,
                                                __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_read_prof.ga_fwd,
                                                __ATOMIC_RELAXED));
}

/* A follower's ReadIndex is one RPC to the leader per caller. The dual
 * hosts follow both groups and serve every GETCHUNKS (two ReadIndexes
 * per 64-chunk window); 9 clients × 16 pull threads put ~300 of those
 * RPCs in flight per group at once, and each waited a heartbeat round
 * at the leader (`getchunks slow … ri_ms=36..53`, Sep 30). Readers that
 * arrive while a round is in flight share the NEXT round (never the one
 * already sent: a commit between its send and this reader's arrival
 * would be missed), so any number of concurrent followers' reads cost
 * two leader RPCs per round trip. Errors are shared by that round's
 * waiters; each caller keeps its own deadline. */
struct host_ri_coal {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int inflight;
    uint64_t started, finished;
    uint64_t last_idx;
    int last_rc, last_hint;
};

/* Group ids are EFS_RAFT_GROUP_SHARD (0) and EFS_RAFT_GROUP_SHARD2 (2). */
static struct host_ri_coal g_ri_coal[HOST_NGROUPS] = {
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, -1},
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, -1},
};

static int host_remote_read_index(struct efs_raft_host *h, uint8_t group,
                                  int leader, const struct timespec *end,
                                  uint64_t *idx, int *hint)
{
    struct host_ri_coal *c;
    uint64_t need;
    int rc;

    if ((group >> 1) >= HOST_NGROUPS || (group & 1u)) {
        struct efs_msg_raft_mkfs_reply rep;

        rc = host_remote_cmd(h, group, NULL, 0, &rep, leader);
        if (rc == EFS_OK) {
            *idx = rep.index;
            *hint = rep.leader_hint;
        }
        return rc;
    }
    c = &g_ri_coal[group >> 1];
    pthread_mutex_lock(&c->mu);
    need = c->started + 1;
    for (;;) {
        if (c->finished >= need) {
            rc = c->last_rc;
            *idx = c->last_idx;
            *hint = c->last_hint;
            pthread_mutex_unlock(&c->mu);
            return rc;
        }
        if (!c->inflight) {
            struct efs_msg_raft_mkfs_reply rep;
            uint64_t round = ++c->started;

            c->inflight = 1;
            pthread_mutex_unlock(&c->mu);
            memset(&rep, 0, sizeof(rep));
            rc = host_remote_cmd(h, group, NULL, 0, &rep, leader);
            pthread_mutex_lock(&c->mu);
            c->inflight = 0;
            c->finished = round;
            c->last_rc = rc;
            c->last_idx = rc == EFS_OK ? rep.index : 0;
            c->last_hint = rc == EFS_OK ? rep.leader_hint : -1;
            pthread_cond_broadcast(&c->cv);
            continue;
        }
        if (pthread_cond_timedwait(&c->cv, &c->mu, end) == ETIMEDOUT &&
            c->finished < need) {
            pthread_mutex_unlock(&c->mu);
            return EFS_ERR_BUSY;
        }
    }
}

/* ReadIndex for one group, batched and lock-free on the hot path. The
 * raft core keeps ONE read round (read_begin resets its acks), so rounds
 * used to be serialized by a host-wide read_mu that every inode handler
 * then held across its whole KV read + propose — one metadata RPC at a
 * time per leader (33 threads queued on it under the 9-host posix suite,
 * lookup 53 ms avg). A reader records commit_index on arrival (`want`),
 * joins a pending round instead of restarting it, and is satisfied by any
 * finished round whose read_index >= want with applied >= read_index. It
 * reads all of that from the published view; h->mu is taken only by the
 * one reader that finds no round pending and none covering it, to
 * read_begin. Waiting is on applied_cv under cv_mu (the pump broadcasts
 * after every cycle), never on h->mu — the pump must keep its heartbeat
 * cadence under 100+ concurrent handlers. Not the leader: forward
 * (blocking peer RPC, no host lock held). */
static int host_read_index(struct efs_raft_host *h, uint8_t group,
                           int *leader_hint)
{
    struct timespec end;
    struct host_view v;
    uint64_t want;
    int prof = read_prof_on();
    uint64_t p0 = prof ? now_us_() : 0;
    int first = 1;

    if (leader_hint)
        *leader_hint = -1;
    host_deadline_us(&end, (long)HOST_READ_TRIES * HOST_TICK_US);
    host_view_get(h, group, &v);
    if (!v.has)
        return EFS_ERR_NOT_PRIMARY;
    want = v.commit;
    for (;;) {
        int rc;

        if (!v.has)
            return EFS_ERR_NOT_PRIMARY;
        if (leader_hint)
            *leader_hint = v.leader;
        if (v.role != EFS_RAFT_LEADER) {
            uint64_t ridx = 0;
            int rhint = -1;
            uint64_t p1;

            if (v.leader == h->raft_id)
                return EFS_ERR_NOT_PRIMARY;
            rc = host_remote_read_index(h, group, v.leader, &end, &ridx,
                                        &rhint);
            p1 = prof ? now_us_() : 0;
            if (prof)
                read_prof_add(&g_read_prof.ri_remote, p1 - p0);
            if (rc == EFS_OK)
                rc = host_wait_applied(h, group, ridx, leader_hint);
            if (prof)
                read_prof_add(&g_read_prof.ri_remote_wait, now_us_() - p1);
            if (rc != EFS_OK)
                return rc;
            if (leader_hint && rhint >= 0)
                *leader_hint = rhint;
            return EFS_OK;
        }
        if (host_view_covers(&v, want)) {
            if (prof)
                read_prof_add(first ? &g_read_prof.ri_leader_covered
                                    : &g_read_prof.ri_leader,
                              now_us_() - p0);
            return EFS_OK;
        }
        first = 0;
        if (!v.read_pending) {
            /* Begin a round — under h->mu, re-validated against the core
             * (another reader may have begun one since the view). */
            struct efs_raft *r;

            pthread_mutex_lock(&h->mu);
            r = group_raft(h, group);
            rc = EFS_OK;
            if (r && efs_raft_role(r) == EFS_RAFT_LEADER &&
                !efs_raft_read_pending(r) && !efs_raft_read_covers(r, want))
                rc = efs_raft_read_begin(r);
            host_publish_group(h, group);
            pthread_mutex_unlock(&h->mu);
            if (rc != EFS_OK)
                return rc;
            host_pump_kick(h);
            host_view_get(h, group, &v);
            continue;
        }
        if (host_past_deadline(&end))
            return EFS_ERR_BUSY;
        /* read_done lands when heartbeat replies arrive — the pump
         * publishes the view and broadcasts applied_cv each cycle. */
        pthread_mutex_lock(&h->cv_mu);
        host_view_get(h, group, &v);
        if (v.has && v.role == EFS_RAFT_LEADER && v.read_pending &&
            !host_view_covers(&v, want)) {
            uint64_t s0 = now_us_();
            host_waiter_sleep(h, group, 1, 0, v.stamp, &end);
            s0 = now_us_() - s0;
            if (s0 > 20000)
                fprintf(stderr, "raft-host: read-sleep us=%llu group=%u "
                        "want=%llu read_idx=%llu applied=%llu pending=%d\n",
                        (unsigned long long)s0, group,
                        (unsigned long long)want,
                        (unsigned long long)v.read_index,
                        (unsigned long long)v.applied, v.read_pending);
        }
        pthread_mutex_unlock(&h->cv_mu);
        host_view_get(h, group, &v);
    }
}

static int host_wait_applied(struct efs_raft_host *h, uint8_t group,
                             uint64_t idx, int *leader_hint)
{
    struct timespec end;
    struct host_view v;

    /* Same overall budget as the old poll loop (HOST_READ_TRIES x
     * HOST_TICK_US), but the pump's applied_cv broadcast wakes us the
     * moment the index applies instead of up to 5 ms later. Lock-free
     * predicate on the published view; sleeps under cv_mu, never h->mu. */
    host_deadline_us(&end, (long)HOST_READ_TRIES * HOST_TICK_US);
    for (;;) {
        host_view_get(h, group, &v);
        if (!v.has) {
            /* No local replica: the leader already waited in submit. */
            return EFS_OK;
        }
        if (leader_hint)
            *leader_hint = v.leader;
        if (v.applied >= idx)
            return EFS_OK;
        if (host_past_deadline(&end)) {
            __atomic_fetch_add(&h->obs_wait_timeouts, 1, __ATOMIC_RELAXED);
            return EFS_ERR_BUSY;
        }
        pthread_mutex_lock(&h->cv_mu);
        host_view_get(h, group, &v);
        if (v.has && v.applied < idx) {
            uint64_t s0 = now_us_();
            host_waiter_sleep(h, group, 0, idx, 0, &end);
            s0 = now_us_() - s0;
            if (s0 > 20000)
                fprintf(stderr, "raft-host: apply-sleep us=%llu group=%u "
                        "idx=%llu applied=%llu\n",
                        (unsigned long long)s0, group,
                        (unsigned long long)idx,
                        (unsigned long long)v.applied);
        }
        pthread_mutex_unlock(&h->cv_mu);
    }
}

/* h->mu held. One fsync covered every append whose record ends at or
 * before the synced offset, on every group this process leads. */
static void host_durable_synced(struct efs_raft_host *h)
{
    uint64_t synced;
    int i;

    if (!h->disk)
        return;
    synced = efs_raft_disk_synced_bytes(h->disk);
    for (i = 0; i < HOST_NGROUPS; i++) {
        struct efs_raft *r = h->g[i].r;
        uint64_t idx;

        if (!h->g[i].hosted || !r)
            continue;
        if (efs_raft_role(r) != EFS_RAFT_LEADER)
            continue;
        idx = efs_raft_disk_covered_index(h->disk, h->g[i].group, synced);
        if (idx)
            (void)efs_raft_durable(r, idx);
    }
}

/* Propose one command. *idx / *term identify the log entry: a waiter must
 * match BOTH against the apply ring (host_apply_rc_locked) — the same index
 * carries a different entry after a leader change. term is 0 when the
 * command was forwarded: the leader matched (idx, term) itself before it
 * replied and its reply rc is the verdict; the local wait on rep.index is
 * only read-your-writes (a committed index is unique). */
static int host_propose(struct efs_raft_host *h, uint8_t group,
                        const uint8_t *cmd, uint32_t clen, uint64_t *idx,
                        uint64_t *term, int *leader_hint)
{
    struct efs_raft *r;
    int rc;
    int lid = -1;
    int quiet = 0;
    uint64_t myidx = 0;
    struct efs_msg_raft_mkfs_reply rep;

    if (term)
        *term = 0;
    /* Join the shared fsync before taking h->mu. Every proposer blocked
     * on the raft lock is already inside the hold, so one fsync covers
     * the batch. The fsync used to run inside disk_append while h->mu
     * was held: 144 mkdir threads each waited out a private fsync
     * (idle p50 8 ms, 9×16 p50 258 ms). A caller that already holds
     * (the report batch) keeps that outer fsync and broadcasts after
     * it. Taking a slot on every proposer, including ones that arrive
     * after the first hold, made idle mkdir wait out unrelated
     * appends (p50 7.1 → 11.9 ms) and did not raise the 144-way rate. */
    if (h->disk && efs_raft_disk_sync_depth(h->disk) == 0 &&
        efs_raft_disk_sync_hold(h->disk) == EFS_OK)
        quiet = 1;
    propose_q_add();
    pthread_mutex_lock(&h->mu);
    r = group_raft(h, group);
    if (r) {
        uint32_t voters = efs_raft_voters(r);
        int solo = voters && (voters & (voters - 1)) == 0;

        lid = efs_raft_leader(r);
        if (leader_hint)
            *leader_hint = lid;
        if (efs_raft_role(r) == EFS_RAFT_LEADER && solo && quiet) {
            /* One voter commits inside propose. Drop the hold first so
             * that commit's append fsyncs before it returns. */
            pthread_mutex_unlock(&h->mu);
            (void)efs_raft_disk_sync_release(h->disk);
            quiet = 0;
            pthread_mutex_lock(&h->mu);
            r = group_raft(h, group);
            if (!r || efs_raft_role(r) != EFS_RAFT_LEADER) {
                pthread_mutex_unlock(&h->mu);
                propose_q_sub();
                rc = EFS_ERR_NOT_PRIMARY;
                return rc;
            }
            lid = efs_raft_leader(r);
            if (leader_hint)
                *leader_hint = lid;
        }
        /* A hold is already open (this caller, or one queued ahead).
         * Append locally and let the pump send one AppendEntries for
         * the whole burst. Broadcasting here, under h->mu, shipped a
         * single entry and the next create in the same directory waited
         * out that round trip — the flat ~150 files/s ceiling. */
        if (efs_raft_role(r) == EFS_RAFT_LEADER && !solo &&
            (quiet || (h->disk && efs_raft_disk_sync_depth(h->disk) > 0))) {
            rc = efs_raft_propose_local(r, cmd, clen, idx);
            if (rc == EFS_OK && term)
                *term = efs_raft_term(r);
            if (rc == EFS_OK && idx)
                myidx = *idx;
            if (rc == EFS_OK)
                rc = efs_raft_submit(r, myidx);
            host_publish_group(h, group);
            pthread_mutex_unlock(&h->mu);
            propose_q_sub();
            if (quiet && rc == EFS_OK) {
                int spins = 0;

                /* Threads already inside host_propose are in propose_q.
                 * Let them append before the fsync; a continuous arrival
                 * of new calls is cut off by the spin cap. */
                while (propose_q_busy() && spins++ < 10000)
                    sched_yield();
                host_pump_kick(h);
                rc = efs_raft_disk_sync_release_wait(h->disk);
                if (rc == EFS_OK) {
                    pthread_mutex_lock(&h->mu);
                    r = group_raft(h, group);
                    if (!r || efs_raft_role(r) != EFS_RAFT_LEADER)
                        rc = EFS_ERR_NOT_PRIMARY;
                    else
                        host_durable_synced(h);
                    if (rc == EFS_OK && term && r)
                        *term = efs_raft_term(r);
                    host_publish_group(h, group);
                    pthread_mutex_unlock(&h->mu);
                }
            } else if (quiet) {
                (void)efs_raft_disk_sync_release(h->disk);
            } else if (rc == EFS_OK && h->disk &&
                       efs_raft_disk_sync_depth(h->disk) == 0) {
                /* The outer hold closed before this append, so the
                 * append fsynced itself. Mark that index durable. */
                pthread_mutex_lock(&h->mu);
                r = group_raft(h, group);
                if (!r || efs_raft_role(r) != EFS_RAFT_LEADER)
                    rc = EFS_ERR_NOT_PRIMARY;
                else
                    rc = efs_raft_durable(r, myidx);
                if (rc == EFS_OK && term && r)
                    *term = efs_raft_term(r);
                host_publish_group(h, group);
                pthread_mutex_unlock(&h->mu);
            }
            host_pump_kick(h);
            return rc;
        }
        if (efs_raft_role(r) == EFS_RAFT_LEADER) {
            rc = efs_raft_propose(r, cmd, clen, idx);
            if (rc == EFS_OK && term)
                *term = efs_raft_term(r);
            /* No hold: append fsynced before propose returned. */
            if (rc == EFS_OK && idx)
                rc = efs_raft_durable(r, *idx);
            host_publish_group(h, group);
            pthread_mutex_unlock(&h->mu);
            propose_q_sub();
            if (quiet)
                (void)efs_raft_disk_sync_release(h->disk);
            host_pump_kick(h);
            return rc;
        }
    }
    pthread_mutex_unlock(&h->mu);
    propose_q_sub();
    if (quiet)
        (void)efs_raft_disk_sync_release(h->disk);
    /* Not the leader: forward to it. host_remote_cmd is a blocking peer
     * RPC; no host lock is held across it. The command is already fully
     * formed and the Raft log + apply-side validation order it against
     * any op that lands meanwhile. */
    /* APPEND's reserved offset is absent from the generic submit reply.
     * A follower's local apply result is not the leader's allocation: its
     * cross-group physical EOF can have advanced independently. Retry the
     * inode RPC on the leader, including a role change after admission. */
    if (cmd && clen && cmd[0] == EFS_MD_CMD_APPEND_RSV)
        return EFS_ERR_NOT_PRIMARY;
    rc = host_remote_cmd(h, group, cmd, clen, &rep, lid);
    if (rc != EFS_OK)
        return rc;
    if (idx)
        *idx = rep.index;
    if (term)
        *term = rep.term;
    if (leader_hint && rep.leader_hint >= 0)
        *leader_hint = rep.leader_hint;
    return EFS_OK;
}

/* h->mu held. The apply layer's verdict for the log entry (idx, term), read
 * from the group's apply-result ring, after host_wait_applied said
 * applied >= idx.
 *   - slot holds idx with the same term: the verdict (OK / BUSY / STALE /
 *     ...). term 0 = forwarded command, the index alone identifies it (the
 *     leader already matched its own term before replying).
 *   - slot holds idx with ANOTHER term: our entry was never committed — the
 *     leader lost the term, the new leader truncated it and committed
 *     something else at idx. EFS_ERR_NOT_PRIMARY: nothing happened, retry
 *     at the new leader. Reading the stranger's OK here is what committed
 *     half a cross-shard txn (I17, Sep 21).
 *   - slot overwritten (more than HOST_APPLY_RC_RING applies inside the
 *     wait window): the verdict is unknown. EFS_ERR_BUSY, never OK —
 *     the old "OK on miss" turned a rejected PREP into a phantom success.
 *     obs_arc_miss counts these. */
static int host_apply_rc_locked(struct efs_raft_host *h, uint8_t group,
                                uint64_t idx, uint64_t term)
{
    struct host_group *g = group_slot(h, group);
    uint64_t s;

    /* Not hosted: the leader's submit reply carried the verdict. The
     * slot exists for every group (attach_group sets g->group and
     * hosted=0), so test hosted, not the pointer: on a single-group
     * host the unhosted slot's empty ring answered every forwarded
     * host_propose_wait with BUSY (raft-mkfs's SALT step, Oct 1 07:20Z,
     * arc_miss +1 per call). */
    if (!g || !g->hosted)
        return EFS_OK;
    s = idx & HOST_APPLY_RC_MASK;
    if (g->arc_idx[s] != idx) {
        h->obs_arc_miss++;
        return EFS_ERR_BUSY;
    }
    if (term && g->arc_term[s] != term) {
        h->obs_arc_term_miss++;
        return EFS_ERR_NOT_PRIMARY;
    }
    return g->arc_rc[s];
}

static int host_apply_extra_locked(struct efs_raft_host *h, uint8_t group,
                                   uint64_t idx, uint64_t term,
                                   uint64_t *extra_out)
{
    struct host_group *g = group_slot(h, group);
    uint64_t s;

    if (!g || !g->hosted)
        return EFS_ERR_BUSY; /* no local ring; the extra is not on the wire */
    s = idx & HOST_APPLY_RC_MASK;
    if (g->arc_idx[s] != idx) {
        h->obs_arc_miss++;
        return EFS_ERR_BUSY;
    }
    if (term && g->arc_term[s] != term) {
        h->obs_arc_term_miss++;
        return EFS_ERR_NOT_PRIMARY;
    }
    if (extra_out)
        *extra_out = g->arc_extra[s];
    return EFS_OK;
}

/* Wait for (idx, term) to apply and return the apply layer's verdict. */
static int host_wait_verdict(struct efs_raft_host *h, uint8_t group,
                             uint64_t idx, uint64_t term, int *hint)
{
    int rc;

    rc = host_wait_applied(h, group, idx, hint);
    if (rc != EFS_OK)
        return rc;
    pthread_mutex_lock(&h->mu);
    rc = host_apply_rc_locked(h, group, idx, term);
    pthread_mutex_unlock(&h->mu);
    return rc;
}

/* Wait for (idx, term) to apply; only checks that OUR entry is the one that
 * applied (NOT_PRIMARY otherwise). The verdict itself is not returned.
 * CREATE must not use this: its apply rejects with BUSY (alloc intent)
 * and EXIST (lost the name), and ignoring those answered the client
 * NOT_FOUND (EIO) or OK (both O_EXCL winners). */
static int host_wait_settled(struct efs_raft_host *h, uint8_t group,
                             uint64_t idx, uint64_t term, int *hint)
{
    struct host_group *g;
    int rc = EFS_OK;

    rc = host_wait_applied(h, group, idx, hint);
    if (rc != EFS_OK || !term)
        return rc;
    pthread_mutex_lock(&h->mu);
    g = group_slot(h, group);
    if (g && g->hosted) {
        uint64_t s = idx & HOST_APPLY_RC_MASK;

        if (g->arc_idx[s] == idx && g->arc_term[s] != term) {
            h->obs_arc_term_miss++;
            rc = EFS_ERR_NOT_PRIMARY;
        }
    }
    pthread_mutex_unlock(&h->mu);
    return rc;
}

static int host_propose_wait_idx(struct efs_raft_host *h, uint8_t group,
                                 const uint8_t *cmd, uint32_t clen, int *hint,
                                 uint64_t *idx_out)
{
    uint64_t idx = 0, term = 0;
    int rc;

    rc = host_propose(h, group, cmd, clen, &idx, &term, hint);
    if (rc != EFS_OK)
        return rc;
    if (idx_out)
        *idx_out = idx;
    /* The index applied; the apply layer's verdict rides the per-group ring.
     * A rejected txn PREP (intent-conflict BUSY / version STALE) must reach
     * the proposer — before this, host_wait_applied only proved the index
     * applied, so a lost conflict looked like success and a cross-shard txn
     * committed with a partial intent set (the nlink lost-update). A
     * forwarded command never reaches here with a conflict: host_propose's
     * forward branch returns the leader's submit reply rc directly. */
    return host_wait_verdict(h, group, idx, term, hint);
}

static int host_propose_wait(struct efs_raft_host *h, uint8_t group,
                             const uint8_t *cmd, uint32_t clen, int *hint)
{
    return host_propose_wait_idx(h, group, cmd, clen, hint, NULL);
}

static int host_propose_wait_ex(struct efs_raft_host *h, uint8_t group,
                                const uint8_t *cmd, uint32_t clen, int *hint,
                                uint64_t *extra_out)
{
    uint64_t idx = 0, term = 0;
    int rc, erc;

    rc = host_propose(h, group, cmd, clen, &idx, &term, hint);
    if (rc != EFS_OK)
        return rc;
    rc = host_wait_applied(h, group, idx, hint);
    if (rc != EFS_OK)
        return rc;
    pthread_mutex_lock(&h->mu);
    rc = host_apply_rc_locked(h, group, idx, term);
    erc = host_apply_extra_locked(h, group, idx, term, extra_out);
    pthread_mutex_unlock(&h->mu);
    if (rc == EFS_OK && extra_out)
        rc = erc;
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
 * (no 1↔2 loop). */
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
        /* The peer's hint is that group's leader (set_inode_rc on OK as
         * well as NOT_PRIMARY); remember it for the next single-group
         * forward. Ambiguous when the request spanned two groups. */
        if (ng == 1 && out->primary_id > 0)
            host_fwd_leader_note(groups[0], (int)out->primary_id - 1);
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            return;
        skip = rid;
    }
}

/* Forwarded directory RPCs keep the client's op-id suffix (I16): the
 * host that finally runs the op is the one that probes and records. */
static uint32_t host_fwd_opid(uint8_t *buf, uint32_t slen,
                              const struct efs_opid_req *q)
{
    if (!q || !efs_opid_req_valid(q))
        return slen;
    efs_opid_req_pack(q, buf + slen);
    return slen + EFS_OPID_WIRE_LEN;
}

static void host_fwd_create(struct efs_raft_host *h, efs_ino_t parent,
                            const char *name, uint32_t mode, uint32_t uid,
                            uint32_t gid, uint32_t flags, uint64_t owner,
                            const struct efs_opid_req *q,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_create) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_create *req = (struct efs_msg_inode_create *)buf;
    uint32_t slen;

    memset(buf, 0, sizeof(buf));
    req->parent = parent;
    strncpy(req->name, name, EFS_MAX_NAME - 1);
    req->mode = mode;
    req->uid = uid;
    req->gid = gid;
    req->flags = flags;
    req->owner = owner;
    slen = host_fwd_opid(buf, sizeof(*req), q);
    host_inode_forward(h, EFS_MSG_INODE_CREATE, buf, slen,
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
                            uint64_t op_seq, struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_append) + EFS_APPEND_OPID_LEN];
    struct efs_msg_inode_append *req = (struct efs_msg_inode_append *)buf;
    uint32_t slen = sizeof(*req);

    memset(buf, 0, sizeof(buf));
    req->ino = ino;
    req->len = len;
    if (sess_uuid) {
        memcpy(buf + slen, sess_uuid, EFS_OPID_UUID_LEN);
        memcpy(buf + slen + EFS_OPID_UUID_LEN, &sess_epoch, 4);
        slen += EFS_SESS_WIRE_LEN;
        memcpy(buf + slen, &op_seq, 8);
        slen += 8;
    }
    host_inode_forward(h, EFS_MSG_INODE_APPEND, buf, slen,
                       EFS_MSG_INODE_APPEND_REPLY, out, groups, ng);
}

static void host_fwd_unlink(struct efs_raft_host *h, efs_ino_t parent,
                            const char *name, int is_dir,
                            const struct efs_opid_req *q,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_unlink) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_unlink *req = (struct efs_msg_inode_unlink *)buf;
    uint32_t slen;

    memset(buf, 0, sizeof(buf));
    req->parent = parent;
    strncpy(req->name, name, EFS_MAX_NAME - 1);
    req->is_dir = is_dir ? 1 : 0;
    slen = host_fwd_opid(buf, sizeof(*req), q);
    host_inode_forward(h, EFS_MSG_INODE_UNLINK, buf, slen,
                       EFS_MSG_INODE_UNLINK_REPLY, out, groups, ng);
}

static void host_fwd_link(struct efs_raft_host *h, efs_ino_t src_ino,
                          efs_ino_t new_parent, const char *new_name,
                          const struct efs_opid_req *q,
                          struct efs_msg_inode_reply *out,
                          const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_link) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_link *req = (struct efs_msg_inode_link *)buf;
    uint32_t slen;

    memset(buf, 0, sizeof(buf));
    req->src_ino = src_ino;
    req->new_parent = new_parent;
    strncpy(req->new_name, new_name, EFS_MAX_NAME - 1);
    slen = host_fwd_opid(buf, sizeof(*req), q);
    host_inode_forward(h, EFS_MSG_INODE_LINK, buf, slen,
                       EFS_MSG_INODE_LINK_REPLY, out, groups, ng);
}

static void host_fwd_rename(struct efs_raft_host *h, efs_ino_t old_parent,
                            const char *old_name, efs_ino_t new_parent,
                            const char *new_name,
                            const struct efs_opid_req *q,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_rename_at) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_rename_at *req = (struct efs_msg_inode_rename_at *)buf;
    uint32_t slen;

    memset(buf, 0, sizeof(buf));
    req->old_parent = old_parent;
    strncpy(req->old_name, old_name, EFS_MAX_NAME - 1);
    req->new_parent = new_parent;
    strncpy(req->new_name, new_name, EFS_MAX_NAME - 1);
    slen = host_fwd_opid(buf, sizeof(*req), q);
    host_inode_forward(h, EFS_MSG_INODE_RENAME_AT, buf, slen,
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
                             uint32_t atime_nsec,
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
    req.atime_nsec = atime_nsec;
    host_inode_forward(h, EFS_MSG_INODE_SETATTR, &req, sizeof(req),
                       EFS_MSG_INODE_SETATTR_REPLY, out, groups, ng);
}

static void host_fwd_getchunks(struct efs_raft_host *h, efs_export_id_t export_id, efs_ino_t ino,
                               uint32_t start, uint32_t max, uint64_t generation,
                               struct efs_msg_inode_getchunks_reply *out,
                               const uint8_t *groups, int ng)
{
    struct efs_msg_inode_getchunks req;
    int tries, rid, skip = -1;

    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.start = start;
    req.max = max;
    req.generation = generation;
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
                           const struct efs_meta_attrs *at, uint64_t hold_owner,
                           efs_ino_t child_ino, uint8_t layout,
                           const struct efs_opid_req *q)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;
    int with_op = q && efs_opid_req_valid(q);

    if (nl == 0 || nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = HOST_CREATE_NAME_OFF + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (child_ino)
        n += 9;
    if (with_op)
        n += HOST_OPID_TRAILER;
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_CREATE;
    out[1] = with_op ? HOST_CREATE_F_OPID : 0;
    wr64be(out + 2, parent);
    wr32be(out + 10, mode);
    wr32be(out + 14, at->uid);
    wr32be(out + 18, at->gid);
    wr64be(out + 22, at->now);
    out[30] = (uint8_t)nl;
    memcpy(out + HOST_CREATE_NAME_OFF, name, nl);
    p = out + HOST_CREATE_NAME_OFF + nl;
    memset(p, 0, EFS_OPID_UUID_LEN + 4);
    if (hold_owner && !S_ISDIR(mode))
        host_hold_uuid(hold_owner, p);
    p += EFS_OPID_UUID_LEN + 4;
    if (child_ino) {
        wr64be(p, child_ino);
        p[8] = layout;
        p += 9;
    }
    if (with_op)
        p += pack_opid_trailer(p, q);
    *len = n;
    return EFS_OK;
}

/* Same encoding as sim pack_unlink. Session bytes are zero (not hosted).
 * An op-id trailer (I16) follows when the command is HOST_OPID_TRAILER
 * bytes longer than the base. */
static int pack_unlink_cmd(uint8_t *out, uint32_t *len, efs_ino_t parent,
                           uint64_t now, const char *name,
                           const struct efs_opid_req *q)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl == 0 || nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = 18 + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (q && efs_opid_req_valid(q))
        n += HOST_OPID_TRAILER;
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_UNLINK;
    wr64be(out + 1, parent);
    wr64be(out + 9, now);
    out[17] = (uint8_t)nl;
    memcpy(out + 18, name, nl);
    p = out + 18 + nl;
    memset(p, 0, EFS_OPID_UUID_LEN + 4);
    p += EFS_OPID_UUID_LEN + 4;
    (void)pack_opid_trailer(p, q);
    *len = n;
    return EFS_OK;
}

static int pack_rmdir_cmd(uint8_t *out, uint32_t *len, efs_ino_t parent,
                          uint64_t now, const char *name,
                          const struct efs_opid_req *q)
{
    int rc = pack_unlink_cmd(out, len, parent, now, name, q);

    if (rc == EFS_OK)
        out[0] = EFS_MD_CMD_RMDIR;
    return rc;
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
    /* +53: lane_bits (W25). Nonzero only on the copy proposed to the
     * other group, whose KV has no inode row. UTIMENS carries no op-id;
     * the remaining 12 bytes are zero. */
    memset(out + 53, 0, EFS_OPID_UUID_LEN + 4);
    wr64be(out + 53, u->lane_bits);
    *len = HOST_UTIMENS_LEN;
    return EFS_OK;
}

/* Same encoding as sim_raft_truncate. Unaligned sizes carry a tail CAS. */
static int pack_truncate_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                             uint64_t now, uint64_t expect_gen, uint64_t size,
                             const struct efs_meta_pub *tail,
                             uint64_t lane_mask, uint8_t flags)
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
    out[62] = flags;
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
                               uint32_t epoch, uint64_t seq)
{
    out[0] = EFS_MD_CMD_APPEND_RSV;
    wr64be(out + 1, ino);
    wr64be(out + 9, alen);
    memset(out + 17, 0, EFS_OPID_UUID_LEN + 4 + 8);
    if (uuid) {
        memcpy(out + 17, uuid, EFS_OPID_UUID_LEN);
        wr32be(out + 33, epoch);
        (void)seq; /* replay cache is host-local; log seq stays 0 */
    }
    *len = HOST_APPEND_RSV_LEN;
    return EFS_OK;
}

static int host_append_replay_get(struct efs_raft_host *h, efs_ino_t ino,
                                  const uint8_t *uuid, uint64_t seq,
                                  uint64_t *off_out)
{
    uint64_t token = 0;
    uint32_t i;

    if (!h || !uuid || seq == 0 || !off_out)
        return -1;
    memcpy(&token, uuid, sizeof(token));
    if (!token)
        return -1;
    pthread_mutex_lock(&h->mu);
    for (i = 0; i < 512; i++) {
        if (h->ap_rep[i].seq == seq && h->ap_rep[i].ino == ino &&
            h->ap_rep[i].token == token) {
            *off_out = h->ap_rep[i].off;
            pthread_mutex_unlock(&h->mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&h->mu);
    return -1;
}

static void host_append_replay_put(struct efs_raft_host *h, efs_ino_t ino,
                                   const uint8_t *uuid, uint64_t seq,
                                   uint64_t off)
{
    uint64_t token = 0;
    uint32_t i;

    if (!h || !uuid || seq == 0)
        return;
    memcpy(&token, uuid, sizeof(token));
    if (!token)
        return;
    pthread_mutex_lock(&h->mu);
    i = h->ap_rep_i++ & 511u;
    h->ap_rep[i].token = token;
    h->ap_rep[i].seq = seq;
    h->ap_rep[i].ino = ino;
    h->ap_rep[i].off = off;
    pthread_mutex_unlock(&h->mu);
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
    /* Session bytes are unused on this path. A span publish stores its
     * range here; a full CAS stores the delta list it observed. Old
     * entries are zeros, which is a full CAS that saw no spans. */
    wr32be(q + 0, p->delta_off);
    wr32be(q + 4, p->delta_len);
    wr32be(q + 8, p->delta_base_n);
    wr64be(q + 12, p->delta_base_seq);
    q += EFS_OPID_UUID_LEN + 4;
    wr64be(q, p->candidate_gen);
    wr64be(q + 8, p->expected_gen);
    wr64be(q + 16, p->content_epoch);
    wr32be(q + 24, p->coding_profile_id);
    q += 28;
    wr64be(q, p->inode_gen);
    wr64be(q + 8, p->mtime_gen);
    q[16] = (p->lane_local ? HOST_PUB_F_LANE_LOCAL : 0) |
            (p->fresh_object ? HOST_PUB_F_FRESH_OBJECT : 0);
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

static int pack_activate_mask_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                                  uint64_t mask)
{
    out[0] = EFS_MD_CMD_ACTIVATE_LANE;
    wr64be(out + 1, ino);
    wr64be(out + 9, mask);
    *len = HOST_ACTIVATE_MASK_LEN;
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

/* PREPARE with a raw kind-specific payload (REDUCE / REDUCE_INO /
 * REDUCE_ADD; see efs_txn_encode_*). pack_prep above is the EXCL/GUARD
 * layout. */
static uint32_t pack_prep_raw(uint8_t *out, int kind, const struct efs_txid *t,
                              const struct efs_txn_parts *p, const uint8_t *key,
                              uint32_t klen, const uint8_t *pay, uint32_t plen)
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
    out[n++] = (uint8_t)(klen >> 8);
    out[n++] = (uint8_t)klen;
    memcpy(out + n, key, klen);
    n += klen;
    memcpy(out + n, pay, plen);
    return n + plen;
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
    ino->alloc_chunks = st->alloc;
}

/* Fresh create/mkdir: no write lanes, no used dir-shards. The inode row
 * IS the getattr. Skipping host_read_inode_lanes + the full collect saves
 * a ReadIndex (often a follower-forward RPC) on every create. */
static void host_stat_from_row(const struct efs_meta_row *row,
                               struct efs_meta_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->ino = row->ino;
    st->generation = row->generation;
    st->mode = row->mode;
    st->nlink = row->nlink;
    st->uid = row->uid;
    st->gid = row->gid;
    st->size = row->base_size;
    st->mtime = row->base_mtime;
    st->atime = row->base_atime;
    st->ctime = row->base_ctime;
}

/* Salt lives on the ROOT shard (group 0, written by MKFS) and on the even
 * group's anchor shard (EFS_MD_CMD_SALT at mkfs time). A node hosting only
 * group 2 cannot ReadIndex group 0, but its local KV carries the SALT
 * record — and the salt is immutable once mkfs returns, so a plain local
 * read is authoritative. A missing record is NOT_FOUND (never a guessed
 * 0: every salted placement would diverge). */
static int host_export_salt(struct efs_raft_host *h, uint64_t *salt, int *hint)
{
    struct efs_raft *r;
    int rc;

    if (h->export_salt_valid) {
        *salt = h->export_salt;
        return EFS_OK;
    }
    pthread_mutex_lock(&h->mu);
    r = group_raft(h, EFS_RAFT_GROUP_SHARD);
    pthread_mutex_unlock(&h->mu);
    if (r) {
        rc = host_read_index(h,
                             efs_raft_shard_group(efs_kv_inode_shard(EFS_ROOT_INO)),
                             hint);
        if (rc != EFS_OK)
            return rc;
    }
    rc = efs_meta_apply_export_salt(h->kv, salt);
    if (rc != EFS_OK)
        return rc;
    h->export_salt = *salt;
    h->export_salt_valid = 1;
    return EFS_OK;
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
        return EFS_INODE_RPC_STALE;
    if (rc == EFS_ERR_INVAL)
        return EFS_INODE_RPC_INVAL;
    if (rc == EFS_ERR_EXIST)
        return EFS_INODE_RPC_EXIST;
    if (rc == EFS_ERR_NAMETOOLONG)
        return EFS_INODE_RPC_INVAL;
    if (rc == EFS_ERR_NOT_EMPTY)
        return EFS_INODE_RPC_NOT_EMPTY;
    if (rc == EFS_ERR_NODATA)
        return EFS_INODE_RPC_NODATA;
    return EFS_INODE_RPC_ERROR;
}

static void set_inode_rc(struct efs_msg_inode_reply *out, int rc, int leader_hint)
{
    out->status = rc_to_inode_status(rc);
    out->primary_id = (leader_hint >= 0) ? (efs_node_id_t)(leader_hint + 1) : 0;
    /* A NOT_PRIMARY with no hint makes the client back off as if an
     * election were running (50 ms << n, 16 tries, EBUSY). On a cluster
     * with stable leaders (Sep 29) that still happened; say which handler
     * produced it, at most once a second. */
    if (rc == EFS_ERR_NOT_PRIMARY && leader_hint < 0) {
        static uint64_t last_us;
        uint64_t now = now_us_();

        if (now - last_us > 1000000ull) {
            last_us = now;
            fprintf(stderr, "raft-host: NOT_PRIMARY without hint from=%p "
                    "(addr2line -e efsd)\n", __builtin_return_address(0));
        }
    }
}

/* 1 when every active lane (file) or used dir-shard (hashed dir) of row
 * is a group this host serves. A lane on the other group is why a
 * single-group host answered NOT_PRIMARY with no hint after the op had
 * already committed (Sep 29, fstor007: setattr and rename). */
static int host_row_lanes_hosted(struct efs_raft_host *h, efs_ino_t ino,
                                 const struct efs_meta_row *row)
{
    uint64_t bits;
    uint32_t i;

    bits = S_ISDIR(row->mode) && row->layout != EFS_META_LAYOUT_LOCAL
               ? row->used_shards
               : row->active_lanes;
    for (i = 0; i < EFS_META_LANES; i++) {
        uint8_t lg;

        if ((bits & (1ULL << i)) == 0)
            continue;
        lg = efs_raft_shard_group(efs_kv_lane_shard(ino, (uint8_t)i));
        if (!host_hosts(h, lg))
            return 0;
    }
    return 1;
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
    rc = efs_meta_apply_get_inode_tx(h->kv, ino, host_txn_coord, h, &row);
    if (rc != EFS_OK)
        return rc;
    bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
               ? row.used_shards
               : row.active_lanes;
    /* One ReadIndex per GROUP, not per lane. Lanes straddle the two
     * groups, so a file written through all 64 lanes has 32 in the
     * other group; on a host that follows that group each
     * host_read_index is a round trip to its leader plus a wait, and
     * this loop issued it 32 times for one stat (Sep 30, du/find on
     * fstor007: 5–7 ms per stat of a >10 MiB file, 0.2 ms for a small
     * one). The read index is a property of the group, not the lane. */
    uint8_t seen = (uint8_t)(1u << ig);
    for (i = 0; i < EFS_META_LANES; i++) {
        uint32_t lsh;
        uint8_t lg;
        int hint = -1;

        if ((bits & (1ULL << i)) == 0)
            continue;
        lsh = efs_kv_lane_shard(ino, (uint8_t)i);
        lg = efs_raft_shard_group(lsh);
        if (lg == ig || (seen & (1u << lg)))
            continue;
        seen |= (uint8_t)(1u << lg);
        /* This host has no view of the lane's group. host_read_index
         * would return NOT_PRIMARY with hint -1 (!v.has), and the client
         * cannot follow that. Name a dual host instead. */
        if (!host_hosts(h, lg)) {
            uint8_t need[2];

            host_need_both(need);
            if (leader_hint)
                *leader_hint = host_pick_peer(h, need, 2, -1);
            return EFS_ERR_NOT_PRIMARY;
        }
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
    snprintf(path, cap, "%s/applied.%u", h->mdraft, group);
}

/* Pump thread only, h->mu NOT required: idx was read under h->mu after
 * this cycle's applies and the KV WAL fsync has already run, so the
 * saved index never passes the KV's durable point. `force` skips the
 * throttle (shutdown). Three fsyncs per pump cycle (WAL + one applied
 * file per group) ran under h->mu at ~290 cycles/s: persist_max ~1 ms
 * of every ~3 ms cycle with proposers queued on the lock. The saved
 * index is only a restart lower bound; apply is idempotent for the
 * replayed tail, so writing it every 10 ms is the same guarantee.
 *
 * The record is 16 bytes: idx, then ~idx. It is pwritten in place at
 * offset 0 of a file the pump keeps open, then fdatasync'd. The
 * open + write + fsync + close + rename of a tmp file per write was
 * 5 329 openat / 4 376 rename / 8 943 fsync on the pump thread in one
 * 120 s posix window (Sep 29). A record whose second word is not the
 * complement of the first is torn and reads as 0 (replay from the log
 * floor, which apply tolerates). */
#define HOST_APPLIED_PERSIST_US 10000ull

static int persist_applied(struct efs_raft_host *h, int gi, uint64_t idx,
                           uint64_t now, int force)
{
    uint8_t buf[16];
    int n;

    if (!h->g[gi].hosted || !h->g[gi].r)
        return EFS_OK;
    if (idx == 0 || idx == h->g[gi].applied_saved)
        return EFS_OK;
    if (!force && now - h->g[gi].applied_saved_us < HOST_APPLIED_PERSIST_US)
        return EFS_OK;
    h->g[gi].applied_saved_us = now;
    if (h->g[gi].applied_fd < 0) {
        char path[EFS_MAX_PATH];

        applied_path(h, h->g[gi].group, path, sizeof(path));
        h->g[gi].applied_fd = open(path, O_RDWR | O_CREAT, 0644);
        if (h->g[gi].applied_fd < 0)
            return EFS_ERR_IO;
    }
    wr64be(buf, idx);
    wr64be(buf + 8, ~idx);
    n = (int)pwrite(h->g[gi].applied_fd, buf, sizeof(buf), 0);
    if (n != (int)sizeof(buf) || fdatasync(h->g[gi].applied_fd) != 0) {
        close(h->g[gi].applied_fd);
        h->g[gi].applied_fd = -1;
        return EFS_ERR_IO;
    }
    h->g[gi].applied_saved = idx;
    return EFS_OK;
}

static int load_applied(struct efs_raft_host *h, int gi, uint64_t *idx)
{
    char path[EFS_MAX_PATH];
    uint8_t buf[16];
    int fd, n;

    *idx = 0;
    applied_path(h, h->g[gi].group, path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return EFS_OK;
    n = (int)read(fd, buf, sizeof(buf));
    close(fd);
    if (n == 8) {
        /* Written by the tmp+rename form; always whole. */
        *idx = rd64be(buf);
        return EFS_OK;
    }
    if (n != (int)sizeof(buf))
        return EFS_OK;
    if (rd64be(buf + 8) != ~rd64be(buf))
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
        h->obs_arc_miss || h->obs_arc_term_miss || h->obs_opid_replay ||
        __atomic_load_n(&h->inbox_drop, __ATOMIC_RELAXED) ||
        __atomic_load_n(&h->obs_pub_n, __ATOMIC_RELAXED)) {
        uint64_t pub_n = __atomic_load_n(&h->obs_pub_n, __ATOMIC_RELAXED);
        uint64_t pub_p50 = 0;
        uint32_t pc, pi, pj;
        uint64_t psamp[64];

        if (pub_n > 0) {
            pc = pub_n < 64 ? (uint32_t)pub_n : 64;
            for (pi = 0; pi < pc; pi++)
                psamp[pi] = __atomic_load_n(
                    &h->obs_pub_ring[(pub_n - pc + pi) % 64],
                    __ATOMIC_RELAXED);
            for (pi = 1; pi < pc; pi++) {
                uint64_t v = psamp[pi];

                pj = pi;
                while (pj > 0 && psamp[pj - 1] > v) {
                    psamp[pj] = psamp[pj - 1];
                    pj--;
                }
                psamp[pj] = v;
            }
            pub_p50 = psamp[pc / 2];
        }
        fprintf(stderr,
                "raft-obs: wait_timeouts=%llu pump_hold_max=%lluus "
                "lock_wait_max=%lluus drain_max=%lluus tick_max=%lluus "
                "apply_max=%lluus persist_max=%lluus applies_in_worst=%llu "
                "arc_miss=%llu arc_term_miss=%llu opid_replay=%llu "
                "fin_q=%llu fin_done=%llu fin_drop=%llu fin_full=%llu "
                "pub_p50=%lluus pub_max=%lluus inbox_drop=%llu\n",
                (unsigned long long)h->obs_wait_timeouts,
                (unsigned long long)h->obs_pump_hold_max_us,
                (unsigned long long)h->obs_wait_max_us,
                (unsigned long long)h->obs_drain_max_us,
                (unsigned long long)h->obs_tick_max_us,
                (unsigned long long)h->obs_apply_max_us,
                (unsigned long long)h->obs_persist_max_us,
                (unsigned long long)h->obs_apply_max_cnt,
                (unsigned long long)h->obs_arc_miss,
                (unsigned long long)h->obs_arc_term_miss,
                (unsigned long long)__atomic_load_n(&h->obs_opid_replay,
                                                    __ATOMIC_RELAXED),
                (unsigned long long)h->fin_queued,
                (unsigned long long)h->fin_done,
                (unsigned long long)h->fin_dropped,
                (unsigned long long)h->fin_full,
                (unsigned long long)pub_p50,
                (unsigned long long)__atomic_exchange_n(&h->obs_pub_max_us, 0,
                                                       __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&h->inbox_drop,
                                                    __ATOMIC_RELAXED));
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

/* The KV shape line the W23 stalled-compactor test samples: one line per
 * ~5 s while any of the four numbers moved (an idle table prints once and
 * then stays quiet). Always-on, like iostats; the snapshot is one short
 * l->mu hold, taken off the pump lock. One pump thread per process, so
 * the last-printed state is a function-local static. */
static void host_kv_obs_dump(struct efs_raft_host *h)
{
    static uint64_t last_ms;
    static struct efs_kv_lsm_stats prev;
    static int printed;
    struct efs_kv_lsm_stats s;
    uint64_t now_ms = now_us_() / 1000ull;

    if (now_ms - last_ms < 5000)
        return;
    last_ms = now_ms;
    if (!h->kv || efs_kv_lsm_stats(h->kv, &s) != EFS_OK)
        return;
    if (printed && s.mt_bytes == prev.mt_bytes && s.l0_bytes == prev.l0_bytes &&
        s.n_l0 == prev.n_l0 && s.n_l1 == prev.n_l1)
        return;
    prev = s;
    printed = 1;
    fprintf(stderr, "kv-obs: mt_bytes=%llu l0_bytes=%llu l0=%u l1=%u\n",
            (unsigned long long)s.mt_bytes, (unsigned long long)s.l0_bytes,
            s.n_l0, s.n_l1);
}

static void *host_pump(void *arg)
{
    struct efs_raft_host *h = arg;
    uint64_t last_tick_us = 0;

    while (h->running) {
        struct pollfd pfd;
        uint64_t sink;
        uint64_t c0 = 0;
        uint64_t applied_now[HOST_NGROUPS];
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
        /* One AppendEntries for everything appended since the last
         * send. Proposers only raise the send ceiling; doing the send
         * under their lock shipped a single entry and the next mkdir
         * waited out that round trip. A peer with a batch in flight is
         * left alone (no pipeline — a pipelined suffix applied twelve
         * thousand entries under this lock). */
        if (!propose_q_busy()) {
            for (i = 0; i < HOST_NGROUPS; i++) {
                if (h->g[i].hosted && h->g[i].r &&
                    efs_raft_role(h->g[i].r) == EFS_RAFT_LEADER)
                    (void)efs_raft_flush(h->g[i].r);
            }
        }
        if (obs)
            c0 = now_us_();
        /* One KV WAL fsync per pump cycle, not per applied entry. Every
         * metadata apply is one or more efs_kv puts and each put fsynced
         * the KV WAL, under h->mu, on every replica: 0.5 ms per entry
         * (EFS_RAFT_OBS apply_max 20 ms / 40 applies on the leader,
         * 130 ms / 256 on a follower; results/measure/20260921-215919-
         * w8-stall-timeline obs-*.txt). The Raft log is already durable
         * before commit, so the KV only has to be durable before
         * persist_applied advances the saved index: hold across the
         * applies, release (= the one fsync) before persist_applied.
         * A crash between release and persist re-applies entries whose
         * puts are already in the KV — the same window per-put fsync had,
         * and apply is idempotent for it. A handler-thread KV write
         * during the hold (session/lock rows) is durable at this cycle's
         * release, at most one cycle later than before. */
        (void)efs_kv_lsm_sync_hold(h->kv);
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
        for (i = 0; i < HOST_NGROUPS; i++) {
            applied_now[i] = h->g[i].hosted && h->g[i].r
                                 ? efs_raft_applied(h->g[i].r) : 0;
            (void)host_maybe_snapshot(h, i);
            if (h->g[i].r)
                h->g[i].voters = efs_raft_voters(h->g[i].r);
        }
        /* Applies above may have advanced commit/applied/read state:
         * publish the lock-free view, then wake the waiters
         * (host_wait_applied / host_read_index) that view satisfies.
         * Waiters check the view under cv_mu before sleeping, so the
         * publish-then-wake order here rules out a lost wakeup. */
        for (i = 0; i < HOST_NGROUPS; i++)
            if (h->g[i].hosted)
                host_publish_view(h, i);
        host_waiters_wake(h);
        if (obs) {
            uint64_t held;
            if (t_wait > h->obs_wait_max_us)
                h->obs_wait_max_us = t_wait;
            if (t_drain > h->obs_drain_max_us)
                h->obs_drain_max_us = t_drain;
            if (t_tick > h->obs_tick_max_us)
                h->obs_tick_max_us = t_tick;
            if (h->obs_apply_cycle_us > h->obs_apply_max_us)
                h->obs_apply_max_us = h->obs_apply_cycle_us;
            if (h->obs_apply_cnt > h->obs_apply_max_cnt)
                h->obs_apply_max_cnt = h->obs_apply_cnt;
            held = t_drain + t_tick;
            if (held > h->obs_pump_hold_max_us)
                h->obs_pump_hold_max_us = held;
            host_obs_dump(h, 0);
        }
        pthread_mutex_unlock(&h->mu);
        /* iostats: is always-on (not EFS_RAFT_OBS-gated); the 5 s cadence
         * lives in efs_iostats_dump. */
        efs_iostats_dump(0);
        host_kv_obs_dump(h);
        /* Durability tail, off the lock: the KV WAL fsync for this
         * cycle's applies, then (throttled) the saved applied index.
         * Order matters — the index must not pass the durable KV. Only
         * the pump applies, so nothing under h->mu depends on this fsync
         * having finished; handler-thread KV writes outside the hold
         * fsync themselves. */
        if (obs)
            c0 = now_us_();
        if (efs_kv_lsm_sync_release(h->kv) != EFS_OK)
            fprintf(stderr, "raft-host: kv wal fsync failed at pump release\n");
        {
            uint64_t pnow = now_us_();

            for (i = 0; i < HOST_NGROUPS; i++)
                (void)persist_applied(h, i, applied_now[i], pnow, 0);
        }
        if (obs) {
            t_persist = now_us_() - c0;
            if (t_persist > h->obs_persist_max_us)
                h->obs_persist_max_us = t_persist;
        }
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
    /* Final index, unthrottled, so a clean stop restarts without replay. */
    {
        uint64_t idx[HOST_NGROUPS];
        int i;

        pthread_mutex_lock(&h->mu);
        for (i = 0; i < HOST_NGROUPS; i++)
            idx[i] = h->g[i].hosted && h->g[i].r
                         ? efs_raft_applied(h->g[i].r) : 0;
        pthread_mutex_unlock(&h->mu);
        (void)efs_kv_lsm_sync_release(h->kv);
        for (i = 0; i < HOST_NGROUPS; i++)
            (void)persist_applied(h, i, idx[i], now_us_(), 1);
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

static void desired_path(const struct efs_raft_host *h, uint8_t group,
                         char *out, size_t n)
{
    snprintf(out, n, "%s/desired.%u", h->mdraft, (unsigned)group);
}

static uint32_t load_desired(struct efs_raft_host *h, uint8_t group,
                             uint32_t fallback)
{
    char path[EFS_MAX_PATH];
    uint8_t buf[4];
    int fd;
    uint32_t v;

    desired_path(h, group, path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return fallback;
    if (read(fd, buf, 4) != 4) {
        close(fd);
        return fallback;
    }
    close(fd);
    v = rd32be(buf);
    return cfg_voters_ok(v) ? v : fallback;
}

static int save_desired(struct efs_raft_host *h, uint8_t group, uint32_t voters)
{
    char path[EFS_MAX_PATH], tmp[EFS_MAX_PATH];
    uint8_t buf[4];
    int fd;

    desired_path(h, group, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    wr32be(buf, voters);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return EFS_ERR_IO;
    if (write(fd, buf, 4) != 4 || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return EFS_ERR_IO;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return EFS_ERR_IO;
    }
    return EFS_OK;
}

/* cfg_voters is C_old (bootstrap map). efs_raft_new overlays persisted
 * membership, so a restart after COLD sees C_new. A first-time learner
 * MUST start with C_old — attaching with C_new would skip joint (I18). */
static int attach_replica(struct efs_raft_host *h, int gi, uint32_t cfg_voters)
{
    struct efs_raft_cfg cfg;
    struct efs_raft_store *st;
    uint8_t group = h->g[gi].group;

    st = efs_raft_disk_group(h->disk, group);
    if (!st)
        return EFS_ERR_IO;
    memset(&cfg, 0, sizeof(cfg));
    cfg.id = h->raft_id;
    cfg.n = h->n;
    cfg.voters = cfg_voters;
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
    cfg.snap_open = host_snap_open;
    cfg.snap_read = host_snap_read;
    cfg.snap_close = host_snap_close;
    cfg.snap_chunk = host_snap_chunk;
    h->g[gi].r = efs_raft_new(&cfg);
    if (!h->g[gi].r)
        return EFS_ERR_NOMEM;
    efs_raft_arm_durable(h->g[gi].r);
    h->g[gi].hosted = 1;
    h->g[gi].voters = efs_raft_voters(h->g[gi].r);
    {
        uint64_t applied = 0;
        if (load_applied(h, gi, &applied) == EFS_OK && applied > 0) {
            (void)efs_raft_restore_applied(h->g[gi].r, applied);
            h->g[gi].applied_saved = efs_raft_applied(h->g[gi].r);
        }
    }
    host_publish_view(h, gi);
    return EFS_OK;
}

static int attach_group(struct efs_raft_host *h, int gi, uint8_t group)
{
    uint32_t boot = group_voters(group, h->n);
    uint32_t want;

    h->g[gi].group = group;
    h->g[gi].voters = boot;
    h->g[gi].host = h;
    h->g[gi].hosted = 0;
    h->g[gi].snap_fd = -1;
    h->g[gi].part_fd = -1;
    h->g[gi].applied_fd = -1;
    h->g[gi].snap_exporting = 0;
    h->g[gi].snap_ready = 0;
    h->g[gi].r = NULL;
    V_STORE(h->g[gi].v_has, 0);
    memset(h->g[gi].arc_idx, 0, sizeof(h->g[gi].arc_idx));
    memset(h->g[gi].arc_rc, 0, sizeof(h->g[gi].arc_rc));
    want = load_desired(h, group, boot);
    h->g[gi].desired = want;
    if (!hosts_group(h->raft_id, boot) && !hosts_group(h->raft_id, want))
        return EFS_OK;
    return attach_replica(h, gi, boot);
}

static void host_cfg_fan_note(struct efs_raft_host *h, uint8_t group,
                              uint32_t voters)
{
    uint8_t cmd[HOST_CFG_LEN];
    struct efs_msg_raft_mkfs_reply rep;
    int rid;

    cmd[0] = EFS_MD_CMD_CFG;
    cmd[1] = EFS_MD_CFG_NOTE;
    wr32be(cmd + 2, voters);
    for (rid = 0; rid < h->n; rid++) {
        if (rid == h->raft_id)
            continue;
        memset(&rep, 0, sizeof(rep));
        (void)host_rpc_submit(h, rid, group, cmd, HOST_CFG_LEN, &rep,
                              HOST_SEND_IO_MS);
    }
}

/* Operator desired ≠ actual (I18). Persist + attach a learner with C_old,
 * then the current leader calls efs_raft_change. Not a control-plane Raft
 * group — that stays sim-only until step 6's remainder is specified. */
static int host_cfg(struct efs_raft_host *h, uint8_t group,
                    const uint8_t *cmd, uint32_t clen, int *hint)
{
    uint32_t voters;
    uint8_t sub;
    struct host_group *s;
    struct efs_raft *r;
    struct efs_msg_raft_mkfs_reply rep;
    int rc, lid = -1;

    if (clen < HOST_CFG_LEN ||
        (group != EFS_RAFT_GROUP_SHARD && group != EFS_RAFT_GROUP_SHARD2))
        return EFS_ERR_INVAL;
    sub = cmd[1];
    voters = rd32be(cmd + 2);
    if (!cfg_voters_ok(voters))
        return EFS_ERR_INVAL;
    if (sub != EFS_MD_CFG_NOTE && sub != EFS_MD_CFG_CHANGE)
        return EFS_ERR_INVAL;
    rc = save_desired(h, group, voters);
    if (rc != EFS_OK)
        return rc;
    pthread_mutex_lock(&h->mu);
    s = group_slot(h, group);
    if (s)
        s->desired = voters;
    if (s && !s->r && hosts_group(h->raft_id, voters))
        rc = attach_replica(h, (int)(s - h->g), group_voters(group, h->n));
    pthread_mutex_unlock(&h->mu);
    if (rc != EFS_OK)
        return rc;
    if (sub == EFS_MD_CFG_NOTE)
        return EFS_OK;
    host_cfg_fan_note(h, group, voters);
    pthread_mutex_lock(&h->mu);
    s = group_slot(h, group);
    r = (s && s->r) ? s->r : NULL;
    if (r) {
        lid = efs_raft_leader(r);
        if (hint)
            *hint = lid;
        if (efs_raft_role(r) == EFS_RAFT_LEADER) {
            rc = efs_raft_change(r, voters);
            s->voters = efs_raft_voters(r);
            pthread_mutex_unlock(&h->mu);
            host_pump_kick(h);
            return rc;
        }
    }
    pthread_mutex_unlock(&h->mu);
    memset(&rep, 0, sizeof(rep));
    rc = host_remote_cmd(h, group, cmd, clen, &rep, lid);
    if (hint && rep.leader_hint >= 0)
        *hint = rep.leader_hint;
    return rc;
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
 * Proposals are leader-only (a 32-marker pass
 * holding it across every raft RTT starved LOOKUP/CREATE — the posix
 * jobs=16 collapse). Scans and fragment I/O stay outside every host lock. */

/* Sep 30: one scan of 32 records and one 16-ack entry per group per
 * second reclaimed 16 fragments/s — 21 hours for one 100 GiB file (2.46M
 * fragments), while every scan held the KV lock. A scan is paid per
 * segment opened, not per record, so it collects 256; the pass repeats
 * while the scan is full and the budget lasts, yielding the lock between
 * scans; an entry carries 128 acks (3+22*128 = 2819 B, no wire cap). */
#define GC_SCAN_MAX   256 /* records collected per scan */
#define REAP_SCAN_MAX 32  /* markers collected per REAP pass */
#define GC_ACK_MAX    EFS_META_GC_ACK_MAX /* 128 items per GC_ACK entry */
#define GC_LOOP_MS    1000
#define GC_FRAG_BUDGET_US 200000 /* frag work per group per loop */
#define GC_FRAG_YIELD_US  1000   /* between scans: let the apply have l->mu */

#define GC_KEY_LEN   24u /* [anchor:2][GC:1][ino:8][gen:8][lane:1][ci:4] */
#define REAP_KEY_LEN 11u /* [anchor:2][REAP:1][ino:8] */

struct gc_scan_ctx {
    int n;
    int full;
    uint32_t skip_len;
    uint8_t skip[GC_KEY_LEN];
    uint8_t keys[GC_SCAN_MAX][GC_KEY_LEN];
    uint8_t vals[GC_SCAN_MAX][EFS_META_GC_VAL];
};

/* D26: last emitted GC key per group, so the next pass scan_from's
 * past the tombstone prefix instead of restarting at the head. */
static uint8_t g_gc_cur[HOST_NGROUPS][GC_KEY_LEN];
static uint32_t g_gc_cur_len[HOST_NGROUPS];

static int gc_gi(uint8_t group)
{
    return group == 0 ? 0 : 1;
}

static int gc_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                      const uint8_t *val, uint32_t vlen)
{
    struct gc_scan_ctx *c = user;

    /* scan_from is inclusive; skip the cursor key we already emitted. */
    if (c->skip_len && klen == c->skip_len &&
        memcmp(key, c->skip, klen) == 0)
        return 0;
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

/* D26: derive the pending-GC watermark — count the live records under
 * one anchor's prefix, once per recovery/import. */
static int gc_count_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    uint64_t *n = user;

    (void)key;
    (void)val;
    if (klen == GC_KEY_LEN && vlen >= EFS_META_GC_VAL)
        (*n)++;
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
    /* The v1 inode/GC namespace belongs to export 1. Recreate its context
     * after restart even when no GET/PUT has occurred. Geometry is inferred
     * from each stored fragment by checksum-conditional deletion. */
    ex = server_export_acquire_or_create_locked(h->s, 1);
    pthread_mutex_unlock(&h->s->lock);
    return ex;
}

/* Delete one fragment on THIS node, checksum-conditional. EFS_OK when the
 * dead bytes are gone afterwards (deleted / absent / slot reused). */
static int host_gc_local_del(struct efs_raft_host *h, struct efs_export *ex,
                             efs_ino_t ino, uint32_t ci, uint32_t fi,
                             uint64_t chunk_gen, const uint8_t *sum)
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
    fid.chunk_generation = chunk_gen;
    efs_store_nvme_bind(&st, &nctx, h->s, ex);
    rc = efs_store_del_if_sum(&st, &fid, sum);
    if (rc == EFS_ERR_EXIST)
        rc = EFS_OK; /* slot reused: the dead generation is already gone */
    return rc;
}

/* Ask the owning node to delete one fragment. EFS_OK = gone/ackable. */
static int host_gc_remote_del(struct efs_raft_host *h, efs_node_id_t node,
                              efs_export_id_t export_id, efs_ino_t ino,
                              uint32_t ci, uint32_t fi, uint64_t chunk_gen,
                              const uint8_t *sum)
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
    req.chunk_generation = chunk_gen;
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


/* Reap markers are the durable retry ledger for unreferenced PUT bodies too.
 * All members must install a death fence and finish their bounded inventory
 * before REAP_DONE removes that ledger. Never use the live-inode collector. */
static int host_gc_inode_nodes(struct efs_raft_host *h, efs_ino_t ino, uint64_t gen)
{
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t count;
    pthread_mutex_lock(&h->s->lock);
    count = h->s->node_count;
    memcpy(nodes, h->s->nodes, count * sizeof(nodes[0]));
    pthread_mutex_unlock(&h->s->lock);
    uint32_t present = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!nodes[i].id || nodes[i].id > (uint32_t)h->n)
            return EFS_ERR_BUSY;
        present |= 1u << (nodes[i].id - 1);
    }
    /* A restart can form a quorum before its final storage member rejoins.
     * Missing membership/address is not proof that its orphan files are gone. */
    int result = present == ((1u << h->n) - 1u) ? EFS_OK : EFS_ERR_BUSY;
    for (uint32_t i = 0; i < count; i++) {
        int rc = EFS_ERR_IO;
        if (nodes[i].id == h->s->id)
            rc = server_gc_inode(h->s, 1, ino, gen);
        else {
            struct efs_conn *pc = server_peer_conn_get(nodes[i].addr, nodes[i].port);
            if (pc) {
                if (pc->kind == EFS_CONN_TCP) {
                    efs_set_recv_timeout(pc->fd, HOST_SEND_IO_MS);
                    efs_set_send_timeout(pc->fd, HOST_SEND_IO_MS);
                }
                struct efs_msg_gc_inode req = {
                    .export_id = 1, .ino = ino, .generation = gen};
                uint8_t type;
                void *reply = NULL;
                uint32_t len = 0;
                if (!efs_conn_send_msg(pc, EFS_MSG_GC_INODE, &req, sizeof(req)) &&
                    !efs_conn_recv_msg(pc, &type, &reply, &len) &&
                    type == EFS_MSG_GC_INODE_REPLY &&
                    len == sizeof(struct efs_msg_gc_inode_reply))
                    rc = ((struct efs_msg_gc_inode_reply *)reply)->rc;
                free(reply);
                if (rc == EFS_OK || rc == EFS_ERR_BUSY) {
                    if (pc->kind == EFS_CONN_TCP) {
                        efs_set_recv_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
                        efs_set_send_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
                    }
                    server_peer_conn_release(nodes[i].addr, nodes[i].port, pc);
                } else
                    server_peer_conn_drop(nodes[i].addr, nodes[i].port, pc);
            }
        }
        if (rc != EFS_OK)
            result = rc;
    }
    return result;
}

/* Propose one GC command and wait for it to apply. The reaper walks the
 * anchor shards of groups this node LEADS, but the commands it drives do
 * not all land in that group: a lane's shard is ish + lane * (odd stride),
 * so every odd lane of an inode lives in the OTHER group. Until Sep 20
 * this was leader-only (NOT_PRIMARY otherwise), which made any inode with
 * an active odd lane unreapable unless one node happened to lead both
 * groups: the pass swept lane 0 (landed), failed lane 1, and retried the
 * whole marker next second — 64 markers × 1/s = 92 % of the raft log was
 * LANE_SWEEP, ~33 entries/s per group with the cluster idle, and every
 * client commit queued behind that fsync stream (mkdir median 103 ms).
 * A foreign-group command is forwarded to that group's leader exactly as a
 * client op would be (host_remote_cmd), then we wait for the local replica
 * to apply it if we host one. No handler lock is taken — one would serialize
 * every inode RPC, and a REAP_SCAN_MAX pass used to hold it across tens of
 * raft RTTs. */
static int host_bg_propose(struct efs_raft_host *h, uint8_t group,
                           const uint8_t *cmd, uint32_t clen, int verdict)
{
    struct efs_raft *r;
    struct efs_msg_raft_mkfs_reply rep;
    uint64_t idx = 0, term = 0;
    int rc, lid = -1;

    pthread_mutex_lock(&h->mu);
    r = group_raft(h, group);
    if (r && efs_raft_role(r) == EFS_RAFT_LEADER) {
        rc = efs_raft_propose(r, cmd, clen, &idx);
        if (rc == EFS_OK)
            term = efs_raft_term(r);
        if (rc == EFS_OK)
            rc = efs_raft_durable(r, idx);
        pthread_mutex_unlock(&h->mu);
        if (rc != EFS_OK)
            return rc;
        host_pump_kick(h);
        /* Recovery's DECIDE ABORT / RESOLVE verdicts decide a txn's fate;
         * they must be OUR entry's verdict, so (idx, term) even when the
         * caller does not want the rc (host_wait_settled). */
        if (!verdict)
            return host_wait_settled(h, group, idx, term, NULL);
        return host_wait_verdict(h, group, idx, term, NULL);
    }
    if (r)
        lid = efs_raft_leader(r);
    pthread_mutex_unlock(&h->mu);
    memset(&rep, 0, sizeof(rep));
    /* The forward path's rep.rc IS the leader's apply verdict. */
    rc = host_remote_cmd(h, group, cmd, clen, &rep, lid);
    if (rc != EFS_OK)
        return rc;
    return host_wait_applied(h, group, rep.index, NULL);
}

static int host_gc_propose(struct efs_raft_host *h, uint8_t group,
                           const uint8_t *cmd, uint32_t clen)
{
    int rc = host_bg_propose(h, group, cmd, clen, 1);
    if (rc != EFS_OK) {
        uint64_t *counter = cmd[0] == EFS_MD_CMD_LANE_SWEEP ? &h->s->gc_sweep_errors :
                            (cmd[0] == EFS_MD_CMD_REAP_DONE || cmd[0] == EFS_MD_CMD_ORPHAN_REAP) ? &h->s->gc_reap_errors :
                            &h->s->gc_ack_errors;
        __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
    }
    return rc;
}


struct orphan_scan {
    uint8_t skip[EFS_KV_KEY_MAX], last[EFS_KV_KEY_MAX];
    uint32_t skip_len, last_len, visited, count;
    uint8_t group;
    int discovery;
    efs_ino_t ino[32];
    uint64_t gen[32];
};
static int orphan_scan_cb(void *user, const uint8_t *key, uint32_t kl,
                            const uint8_t *value, uint32_t vl)
{
    struct orphan_scan *scan = user;
    if (kl == scan->skip_len && !memcmp(key, scan->skip, kl)) return 0;
    if (kl > sizeof(scan->last)) return -1;
    if (scan->count == 32 || scan->visited == 4096) return 1;
    memcpy(scan->last, key, kl); scan->last_len = kl; scan->visited++;
    efs_ino_t ino = 0; uint64_t gen = 0;
    if (scan->discovery) {
        struct efs_meta_row row;
        if (kl != 11 || key[2] != EFS_KV_KIND_INODE ||
            efs_raft_shard_group(((uint32_t)key[0]<<8)|key[1]) != scan->group)
            return 0;
        if (efs_meta_unpack_inode(value, vl, &row) != EFS_OK) return -1;
        if (row.nlink) return 0;
        ino = row.ino; gen = row.generation;
    } else {
        if (kl != 11 || vl != 8) return -1;
        ino = rd64be(key + 3); gen = rd64be(value);
    }
    scan->ino[scan->count] = ino; scan->gen[scan->count++] = gen;
    return 0;
}
static void host_gc_orphan_pass(struct efs_raft_host *h, uint8_t group, uint32_t anchor)
{
    int gi = gc_gi(group);
    for (int discovery = 0; discovery < 2; discovery++) {
        if (discovery && h->orphan_discovery_done[gi]) continue;
        uint8_t prefix[3] = {(uint8_t)(anchor>>8),(uint8_t)anchor,EFS_KV_KIND_ORPHAN};
        uint8_t *cursor = discovery ? h->orphan_discovery_cursor[gi] : h->orphan_cursor[gi];
        uint32_t *len = discovery ? &h->orphan_discovery_len[gi] : &h->orphan_cursor_len[gi];
        struct orphan_scan scan = {.group=group, .discovery=discovery, .skip_len=*len};
        memcpy(scan.skip,cursor,*len);
        int rc = efs_kv_scan_from(h->kv, discovery ? NULL : prefix, discovery ? 0 : 3,
                                   *len ? cursor : NULL, *len, orphan_scan_cb, &scan);
        if (rc < 0) {
            __atomic_fetch_add(&h->s->gc_scan_errors,1,__ATOMIC_RELAXED); continue;
        }
        if (!discovery) __atomic_store_n(&h->s->gc_orphan_seen[gi],scan.count,__ATOMIC_RELAXED);
        if (rc == 0) {
            *len = 0;
            if (discovery) h->orphan_discovery_done[gi] = 1;
        } else if (scan.last_len) {
            memcpy(cursor,scan.last,scan.last_len); *len=scan.last_len;
        }
        for (uint32_t i=0;i<scan.count && h->gc_running;i++) {
            uint8_t cmd[17]={EFS_MD_CMD_ORPHAN_REAP};
            wr64be(cmd+1,scan.ino[i]);wr64be(cmd+9,scan.gen[i]);
            (void)host_gc_propose(h,group,cmd,sizeof(cmd));
        }
    }
}

/* Txn finisher. A coordinator that proposed DECIDE COMMIT and then hit
 * the 400 ms apply wait (a compaction stall: apply_max 1.6 s on two
 * replicas at once, results/posix/20260926-0330-diag) returned BUSY to
 * the client with its intents still pending — an EXCL intent on the
 * child shard's ALLOC key makes every log-path create/mkdir on that
 * shard BUSY until the 5 s recovery scan resolves it (txn-recover ...
 * age=6.0s -> COMMIT (resolved)). One stall poisoned 79 txns for 6 s.
 * The coordinator knows the txn; only its wait ran out. It hands the
 * txn here and this thread reads the decision record once the stall
 * clears, then proposes the RESOLVEs — the same commands recovery would
 * send, so racing recovery is harmless (RESOLVE is idempotent). No
 * decision within HOST_FIN_MAX_NS → drop; recovery ABORTs. */
#define HOST_FIN_MAX_NS (10ull * 1000000000ull)
#define HOST_FIN_POLL_US 5000

static void host_fin_add(struct efs_raft_host *h, const struct efs_txid *t,
                         const struct efs_txn_parts *p, uint32_t coord)
{
    struct host_fin_item *it;

    if (!h->fin || !h->fin_running)
        return;
    pthread_mutex_lock(&h->fin_mu);
    if (h->fin_n >= HOST_FIN_MAX) {
        h->fin_full++;
        pthread_mutex_unlock(&h->fin_mu);
        return;
    }
    it = &h->fin[(h->fin_head + h->fin_n) % HOST_FIN_MAX];
    it->t = *t;
    it->parts = *p;
    it->coord = coord;
    it->born_ns = now_ns();
    h->fin_n++;
    h->fin_queued++;
    pthread_cond_signal(&h->fin_cv);
    pthread_mutex_unlock(&h->fin_mu);
}

static void host_fin_one(struct efs_raft_host *h, const struct host_fin_item *it)
{
    uint8_t cmd[22];
    uint8_t cg = efs_raft_shard_group(it->coord);
    int dec = EFS_TXN_ABORT, rc, i, hint = -1;

    for (;;) {
        if (!h->fin_running)
            return;
        if (now_ns() - it->born_ns > HOST_FIN_MAX_NS) {
            h->fin_dropped++;
            return;
        }
        if (!host_hosts(h, cg)) {
            h->fin_dropped++;
            return;
        }
        rc = host_read_index(h, cg, &hint);
        if (rc == EFS_OK)
            rc = efs_txn_decision_get(h->kv, it->coord, &it->t, &dec);
        if (rc == EFS_OK)
            break;
        if (rc != EFS_ERR_NOT_FOUND && rc != EFS_ERR_BUSY &&
            rc != EFS_ERR_NOT_PRIMARY) {
            h->fin_dropped++;
            return;
        }
        usleep(HOST_FIN_POLL_US);
    }
    for (i = 0; i < it->parts.n; i++) {
        uint32_t sh = it->parts.shard[i];

        pack_resolve(cmd, &it->t, sh, dec);
        for (;;) {
            if (!h->fin_running)
                return;
            if (now_ns() - it->born_ns > HOST_FIN_MAX_NS) {
                h->fin_dropped++;
                return;
            }
            /* verdict=1: a rejected RESOLVE must not count as done.
             * BUSY here is the same 400 ms apply wait that stranded the
             * txn; ride it out until the deadline, then recovery. */
            rc = host_bg_propose(h, efs_raft_shard_group(sh), cmd, 22, 1);
            if (rc == EFS_OK)
                break;
            if (rc != EFS_ERR_BUSY && rc != EFS_ERR_NOT_PRIMARY) {
                h->fin_dropped++;
                fprintf(stderr, "raft-host: txn-finish coord=%u resolve "
                        "sh=%u dec=%d rc=%d (recovery)\n",
                        it->coord, sh, dec, rc);
                return;
            }
            usleep(HOST_FIN_POLL_US);
        }
    }
    h->fin_done++;
}

static void *host_fin_thread(void *arg)
{
    struct efs_raft_host *h = arg;
    struct host_fin_item it;

    while (h->fin_running) {
        pthread_mutex_lock(&h->fin_mu);
        while (h->fin_running && h->fin_n == 0)
            pthread_cond_wait(&h->fin_cv, &h->fin_mu);
        if (!h->fin_running) {
            pthread_mutex_unlock(&h->fin_mu);
            break;
        }
        it = h->fin[h->fin_head];
        h->fin_head = (h->fin_head + 1) % HOST_FIN_MAX;
        h->fin_n--;
        pthread_mutex_unlock(&h->fin_mu);
        host_fin_one(h, &it);
    }
    return NULL;
}

/* ---- stranded-transaction recovery (architecture §7.2, L5) ----------------
 *
 * A coordinator that dies, or gives up, between its PREPAREs and the last
 * RESOLVE leaves INTENT/GUARD/REDUCE records on the participants. Every
 * later EXCL/GUARD/log-path op on those keys is BUSY for as long as the
 * records exist, and nothing else removes them. Sep 21
 * (results/measure/20260921-w8-orphans): 105 intents, 45 of them ALLOC keys,
 * up to 5 000 s old, from txns whose DECIDE/RESOLVE hit the 400 ms apply
 * wait while efs_txn_resolve was still a whole-shard scan; each poisoned
 * shard turned one of nine fresh-parent mkdirs into a 10.4 s EBUSY.
 *
 * Which shards to look at comes from the apply path, not from a sweep: every
 * PREPARE applied on shard S marks S (host_rec_mark, first-mark time kept).
 * Once a mark is HOST_REC_AGE_NS old the leader of S's group scans S's
 * three txn-record prefixes; nothing pending clears the mark, only young
 * records re-arm it. So an idle cluster scans nothing, a busy shard is
 * scanned once per age period, and a fresh process (all shards marked at
 * start) walks the table once, HOST_REC_SHARDS shards per pass with a
 * yield between scans. The first version swept 512 shards per second
 * unconditionally: 1 536 prefix scans × ~0.3 ms, each taking the KV lock
 * the apply path needs, put mkdir back at 100 ms median
 * (results/measure/20260921-w8-orphans/sweep-regression.txt).
 *
 * For each pending txn older than HOST_REC_AGE_NS the pass (a) proposes
 * DECIDE ABORT at the coordinator — idempotent when ABORT is already there,
 * PROTO when COMMIT is (the coordinator got there first, and its RESOLVEs
 * are what went missing) — then (b) proposes RESOLVE with the established
 * decision to every participant shard the record names. Both commands
 * forward to whichever node leads the target group, so the pass needs no
 * local replica of the coordinator's group. A live txn is never touched:
 * one takes tens of ms, the threshold is seconds. */
#define HOST_REC_AGE_NS  (5ull * 1000000000ull)
#define HOST_REC_SHARDS  64u  /* due shards scanned per GC pass */
#define HOST_REC_MAX     32u  /* txns handled per shard per pass */
#define HOST_REC_YIELD_US 1000

static void host_rec_mark(struct efs_raft_host *h, uint32_t shard)
{
    shard &= EFS_KV_SHARD_MASK;
    pthread_mutex_lock(&h->rec_mu);
    if (h->rec_mark[shard] == 0)
        h->rec_mark[shard] = now_ns();
    pthread_mutex_unlock(&h->rec_mu);
}

static void host_rec_mark_all(struct efs_raft_host *h)
{
    uint64_t now = now_ns();
    uint32_t s;

    pthread_mutex_lock(&h->rec_mu);
    for (s = 0; s <= EFS_KV_SHARD_MASK; s++)
        h->rec_mark[s] = now;
    pthread_mutex_unlock(&h->rec_mu);
}

static uint64_t txid_ns(const struct efs_txid *t)
{
    uint64_t a;

    memcpy(&a, t->bytes, 8); /* fill_txid: now_ns(), host byte order */
    return a;
}

static int host_leads(struct efs_raft_host *h, uint8_t group)
{
    struct efs_raft *r;
    int lead;

    pthread_mutex_lock(&h->mu);
    r = group_raft(h, group);
    lead = r && efs_raft_role(r) == EFS_RAFT_LEADER;
    pthread_mutex_unlock(&h->mu);
    return lead;
}

static void host_txn_recover_one(struct efs_raft_host *h, uint32_t shard,
                                 const struct efs_txn_pending_rec *rec)
{
    uint8_t cmd[22];
    uint32_t coord, i, nsh, shards[EFS_TXN_MAX_PART + 1];
    int dec = EFS_TXN_ABORT, rc, j;

    h->rec_found++;
    coord = efs_txn_coordinator(&rec->t, &rec->parts);
    pack_decide(cmd, &rec->t, coord, EFS_TXN_ABORT);
    rc = host_bg_propose(h, efs_raft_shard_group(coord), cmd, 22, 1);
    if (rc == EFS_ERR_PROTO) {
        dec = EFS_TXN_COMMIT; /* committed; only its RESOLVEs were lost */
    } else if (rc != EFS_OK) {
        h->rec_fail++;
        fprintf(stderr, "raft-host: txn-recover shard=%u coord=%u age=%.1fs "
                "decide-abort rc=%d (retry next pass)\n", shard, coord,
                (double)(now_ns() - txid_ns(&rec->t)) / 1e9, rc);
        return;
    } else {
        h->rec_aborted++;
    }
    /* Every participant the record names, plus the shard we found it on
     * (a record's part list always includes its own shard, but do not
     * depend on it). */
    nsh = 0;
    for (i = 0; i < rec->parts.n && nsh < EFS_TXN_MAX_PART + 1; i++) {
        for (j = 0; j < (int)nsh; j++)
            if (shards[j] == rec->parts.shard[i])
                break;
        if (j == (int)nsh)
            shards[nsh++] = rec->parts.shard[i];
    }
    for (j = 0; j < (int)nsh; j++)
        if (shards[j] == shard)
            break;
    if (j == (int)nsh)
        shards[nsh++] = shard;
    for (i = 0; i < nsh; i++) {
        uint32_t sh = shards[i];

        pack_resolve(cmd, &rec->t, sh, dec);
        rc = host_bg_propose(h, efs_raft_shard_group(sh), cmd, 22, 1);
        if (rc != EFS_OK) {
            h->rec_fail++;
            fprintf(stderr, "raft-host: txn-recover shard=%u resolve sh=%u "
                    "dec=%d rc=%d (retry next pass)\n", shard, sh, dec, rc);
            return;
        }
    }
    h->rec_resolved++;
    fprintf(stderr, "raft-host: txn-recover shard=%u coord=%u parts=%u "
            "age=%.1fs -> %s\n", shard, coord, rec->parts.n,
            (double)(now_ns() - txid_ns(&rec->t)) / 1e9,
            dec == EFS_TXN_COMMIT ? "COMMIT (resolved)" : "ABORT");
}

static void host_txn_recover_pass(struct efs_raft_host *h)
{
    struct efs_txn_pending_rec recs[HOST_REC_MAX];
    uint64_t now = now_ns();
    uint32_t shard, n, i, done = 0;
    int lead[2];

    if (!h->kv)
        return;
    lead[0] = host_leads(h, EFS_RAFT_GROUP_SHARD);
    lead[1] = host_leads(h, EFS_RAFT_GROUP_SHARD2);
    if (!lead[0] && !lead[1])
        return;
    for (shard = 0; shard <= EFS_KV_SHARD_MASK && done < HOST_REC_SHARDS &&
                    h->gc_running && h->running; shard++) {
        uint64_t mark;

        if (!lead[(shard & 1u) ? 0 : 1])
            continue;
        pthread_mutex_lock(&h->rec_mu);
        mark = h->rec_mark[shard];
        pthread_mutex_unlock(&h->rec_mu);
        if (mark == 0 || now - mark < HOST_REC_AGE_NS)
            continue;
        done++;
        h->rec_scans++;
        n = 0;
        if (efs_txn_scan_pending(h->kv, shard, recs, HOST_REC_MAX, &n) != EFS_OK)
            continue; /* mark stays; retried next pass */
        for (i = 0; i < n && h->gc_running && h->running; i++) {
            uint64_t born = txid_ns(&recs[i].t);

            if (born > now || now - born < HOST_REC_AGE_NS)
                continue;
            host_txn_recover_one(h, shard, &recs[i]);
        }
        /* Clean: clear the mark unless a PREPARE landed meanwhile (mark
         * moved). Anything found — young, recovered, or a failed recovery
         * — re-arms from now so the shard is looked at again in one age
         * period and a recovery is verified gone. */
        pthread_mutex_lock(&h->rec_mu);
        if (h->rec_mark[shard] == mark)
            h->rec_mark[shard] = n ? now : 0;
        pthread_mutex_unlock(&h->rec_mu);
        usleep(HOST_REC_YIELD_US); /* let the apply path have the KV lock */
    }
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
            rc = host_gc_local_del(h, ex, ino, ci, (uint32_t)fi, gen,
                                   sums[fi]);
        else
            rc = host_gc_remote_del(h, nodes[fi], ex->id, ino, ci,
                                    (uint32_t)fi, gen, sums[fi]);
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
    if (prc != EFS_OK || src < 0) {
        __atomic_fetch_add(&h->s->gc_scan_errors, 1, __ATOMIC_RELAXED);
        return;
    }
    unsigned gi = group == 0 ? 0 : 1;
    __atomic_store_n(&h->s->gc_reap_seen[gi], c.n, __ATOMIC_RELAXED);
    __atomic_store_n(&h->s->gc_reap_capped[gi], src > 0, __ATOMIC_RELAXED);
    uint64_t first = c.n ? c.ino[0] : 0;
    if (__atomic_load_n(&h->s->gc_first_reap[gi], __ATOMIC_RELAXED) != first) {
        __atomic_store_n(&h->s->gc_first_reap[gi], first, __ATOMIC_RELAXED);
        __atomic_store_n(&h->s->gc_first_seen_us[gi], first ? efs_iostats_now_us() : 0, __ATOMIC_RELAXED);
    }
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
        /* Include losing/never-published PUT objects. Namespace deletion
         * alone is insufficient evidence of physical reclamation. */
        if (host_gc_inode_nodes(h,c.ino[i],c.gen[i]) != EFS_OK)
            continue;
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
static int host_gc_ack_flush(struct efs_raft_host *h, uint8_t group,
                             struct efs_gc_ack_item *acks, int *nack)
{
    uint8_t cmd[3 + GC_ACK_MAX * 22];
    int j, off = 3, rc;

    if (*nack <= 0)
        return EFS_OK;
    cmd[0] = EFS_MD_CMD_GC_ACK;
    cmd[1] = (uint8_t)(*nack >> 8);
    cmd[2] = (uint8_t)*nack;
    for (j = 0; j < *nack; j++) {
        wr64be(cmd + off, acks[j].ino);
        wr64be(cmd + off + 8, acks[j].gen);
        cmd[off + 16] = acks[j].lane;
        wr32be(cmd + off + 17, acks[j].ci);
        cmd[off + 21] = acks[j].frag;
        off += 22;
    }
    rc = host_gc_propose(h, group, cmd, (uint32_t)off);
    if (env_on("EFS_GC_DBG"))
        fprintf(stderr, "raft-host: gc ack flush group=%u n=%d rc=%d\n",
                group, *nack, rc);
    *nack = 0;
    return rc;
}

static void host_gc_frag_pass(struct efs_raft_host *h, uint8_t group,
                              uint32_t anchor)
{
    static struct gc_scan_ctx c; /* one GC thread; 35 KB */
    struct efs_gc_ack_item acks[GC_ACK_MAX];
    struct efs_export *ex = NULL;
    uint8_t prefix[3];
    uint32_t plen = 0;
    uint64_t t0 = now_us_();
    int64_t pend;
    int nack = 0, scans = 0, recs = 0;
    int i;

    int prc, src;

    prc = efs_kv_key_gc_prefix(anchor, prefix, &plen);
    if (prc != EFS_OK)
        return;
    /* D26: the pending-GC watermark gates the scan. Unknown (process
     * start, snapshot import): derive it once by counting the prefix.
     * Zero: nothing to collect — an empty pass is this peek, no scan. */
    pend = efs_meta_gc_pending_peek(anchor);
    if (pend < 0) {
        uint64_t n = 0;

        src = efs_kv_scan_prefix(h->kv, prefix, plen, gc_count_cb, &n);
        if (src < 0) {
            __atomic_fetch_add(&h->s->gc_scan_errors, 1, __ATOMIC_RELAXED);
            return;
        }
        efs_meta_gc_pending_derived(anchor, n);
        pend = efs_meta_gc_pending_peek(anchor);
        if (env_on("EFS_GC_DBG"))
            fprintf(stderr, "raft-host: gc watermark group=%u anchor=%u derived=%llu\n",
                    group, anchor, (unsigned long long)n);
    }
    __atomic_store_n(&h->s->gc_pending[group == 0 ? 0 : 1], (uint64_t)pend, __ATOMIC_RELAXED);
    __atomic_fetch_or(&h->s->gc_sampled_mask, 1u << (group == 0 ? 0 : 1), __ATOMIC_RELAXED);
    if (pend == 0) {
        g_gc_cur_len[gc_gi(group)] = 0;
        return;
    }
    for (;;) {
        int gi = gc_gi(group);
        int wrapped = 0;

        memset(&c, 0, sizeof(c));
        if (g_gc_cur_len[gi]) {
            memcpy(c.skip, g_gc_cur[gi], g_gc_cur_len[gi]);
            c.skip_len = g_gc_cur_len[gi];
        }
        /* D26: resume after the last emitted key (scan_from is inclusive,
         * gc_scan_cb drops the start key). A wrap (0 live keys past the
         * cursor) restarts at the prefix head once per pass. */
        src = efs_kv_scan_from(h->kv, prefix, plen,
                               g_gc_cur_len[gi] ? g_gc_cur[gi] : NULL,
                               g_gc_cur_len[gi], gc_scan_cb, &c);
        scans++;
        if (!ex)
            ex = host_gc_export(h);
        if (env_on("EFS_GC_DBG"))
            fprintf(stderr, "raft-host: gc frag pass group=%u anchor=%u prc=%d src=%d records=%d ex=%p cur=%u\n",
                    group, anchor, prc, src, c.n, (void *)ex, g_gc_cur_len[gi]);
        /* src > 0 is the scan callback's "batch full" stop, not an error.
         * No export yet (pre-mkfs / post-restart before a PUT or GET):
         * nothing to delete under — do not advance the cursor. */
        if (src < 0) {
            __atomic_fetch_add(&h->s->gc_scan_errors, 1, __ATOMIC_RELAXED);
            break;
        }
        if (c.n == 0) {
            if (g_gc_cur_len[gi]) {
                g_gc_cur_len[gi] = 0;
                wrapped = 1;
            }
            if (!wrapped || !h->gc_running) {
                /* An empty scan that started at the prefix head covers
                 * the anchor; if a wrap preceded it the pair covered it
                 * too. Nothing was emitted either way, so a drifted
                 * watermark (derives only ever overshoot) clamps back to
                 * zero and the next pass is a peek. A note that raced
                 * the scans changed the count and zero_if declines. */
                if (!recs && h->gc_running &&
                    efs_meta_gc_pending_zero_if(anchor, pend) &&
                    env_on("EFS_GC_DBG"))
                    fprintf(stderr, "raft-host: gc watermark group=%u anchor=%u drained\n",
                            group, anchor);
                break;
            }
            continue;
        }
        if (!ex) {
            __atomic_fetch_add(&h->s->gc_missing_exports, 1, __ATOMIC_RELAXED);
            break;
        }
        for (i = 0; i < c.n && h->gc_running && h->running; i++) {
            /* Room for one whole record's acks, so a record is never
             * split across two entries (the apply folds a record's
             * fragments within one batch). */
            if (nack + EFS_NUM_FRAGMENTS > GC_ACK_MAX &&
                host_gc_ack_flush(h, group, acks, &nack) != EFS_OK)
                goto out;
            host_gc_record(h, ex, c.keys[i], c.vals[i], acks, &nack);
            recs++;
        }
        memcpy(g_gc_cur[gi], c.keys[c.n - 1], GC_KEY_LEN);
        g_gc_cur_len[gi] = GC_KEY_LEN;
        if (host_gc_ack_flush(h, group, acks, &nack) != EFS_OK)
            break;
        /* The records just acked are still in the KV until the entry
         * applies; a rescan from the cursor starts after them. Stop
         * when the scan was not full, the budget is spent, or the
         * host is going down; otherwise yield the KV lock and rescan. */
        if (!c.full || !h->gc_running || !h->running ||
            now_us_() - t0 > GC_FRAG_BUDGET_US)
            break;
        usleep(GC_FRAG_YIELD_US);
    }
out:
    pend = efs_meta_gc_pending_peek(anchor);
    __atomic_store_n(&h->s->gc_pending[group == 0 ? 0 : 1], (uint64_t)pend, __ATOMIC_RELAXED);
    if (ex)
        server_export_put(h->s, ex);
    if (recs > 0 && env_on("EFS_RAFT_OBS"))
        fprintf(stderr, "raft-host: gc-frag group=%u scans=%d records=%d ms=%llu\n",
                group, scans, recs,
                (unsigned long long)((now_us_() - t0) / 1000));
}

static void host_dir_spread_pass(struct efs_raft_host *h);

static void host_snap_unlink_other(struct efs_raft_host *h, uint8_t group,
                                   const char *keep)
{
    char prefix[32];
    DIR *d;
    struct dirent *de;
    const char *base;

    base = strrchr(keep, '/');
    base = base ? base + 1 : keep;
    snprintf(prefix, sizeof(prefix), "snap-%u-", group);
    d = opendir(h->mdraft);
    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        char path[EFS_MAX_PATH];
        size_t n;

        if (strncmp(de->d_name, prefix, strlen(prefix)) != 0)
            continue;
        n = strlen(de->d_name);
        if (n < 4 || strcmp(de->d_name + n - 4, ".kvx") != 0)
            continue;
        if (strcmp(de->d_name, base) == 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", h->mdraft, de->d_name);
        unlink(path);
    }
    closedir(d);
}

static void host_snap_drop_parts(struct efs_raft_host *h)
{
    DIR *d;
    struct dirent *de;

    d = opendir(h->mdraft);
    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        char path[EFS_MAX_PATH];
        size_t n = strlen(de->d_name);

        if (strncmp(de->d_name, "snap-", 5) != 0)
            continue;
        if (n < 5 || strcmp(de->d_name + n - 5, ".part") != 0) {
            /* W14.3: a killed export leaves snap-*.kvx.tmp. Nothing is
             * in progress at start, so every one of these is stale. */
            if (n < 8 || strcmp(de->d_name + n - 8, ".kvx.tmp") != 0)
                continue;
        }
        snprintf(path, sizeof(path), "%s/%s", h->mdraft, de->d_name);
        unlink(path);
    }
    closedir(d);
}

/* GC thread. The view was pinned on the pump at the snap index. */
static void host_snap_export_pass(struct efs_raft_host *h)
{
    int gi;

    for (gi = 0; gi < HOST_NGROUPS && h->gc_running; gi++) {
        struct efs_kv_lsm_view *view;
        char path[EFS_MAX_PATH];
        uint64_t incl, t0;
        uint8_t group;
        int rc;

        pthread_mutex_lock(&h->snap_mu);
        if (!h->g[gi].snap_exporting || !h->g[gi].snap_view) {
            pthread_mutex_unlock(&h->snap_mu);
            continue;
        }
        view = h->g[gi].snap_view;
        h->g[gi].snap_view = NULL;
        incl = h->g[gi].snap_incl;
        group = h->g[gi].group;
        snprintf(path, sizeof(path), "%s", h->g[gi].snap_path);
        pthread_mutex_unlock(&h->snap_mu);
        t0 = now_us_();
        fprintf(stderr, "raft-snap: start group=%u incl=%llu\n", group,
                (unsigned long long)incl);
        rc = efs_kv_lsm_view_export(view, group, path);
        efs_kv_lsm_view_unpin(view);
        pthread_mutex_lock(&h->snap_mu);
        if (h->g[gi].snap_incl != incl) {
            unlink(path);
            h->g[gi].snap_exporting = 0;
        } else if (rc != EFS_OK) {
            fprintf(stderr, "raft-snap: export failed group=%u rc=%d\n",
                    group, rc);
            h->g[gi].snap_exporting = 0;
            h->g[gi].snap_ready = 0;
        } else {
            struct stat st;

            if (h->g[gi].snap_fd >= 0)
                close(h->g[gi].snap_fd);
            h->g[gi].snap_fd = open(path, O_RDONLY);
            if (h->g[gi].snap_fd < 0 || fstat(h->g[gi].snap_fd, &st) != 0) {
                if (h->g[gi].snap_fd >= 0) {
                    close(h->g[gi].snap_fd);
                    h->g[gi].snap_fd = -1;
                }
                h->g[gi].snap_exporting = 0;
                h->g[gi].snap_ready = 0;
                fprintf(stderr, "raft-snap: open failed group=%u\n", group);
            } else {
                h->g[gi].snap_bytes = (uint64_t)st.st_size;
                h->g[gi].snap_ready = 1;
                h->g[gi].snap_exporting = 0;
                fprintf(stderr, "raft-snap: end group=%u incl=%llu bytes=%llu "
                        "ms=%llu\n", group, (unsigned long long)incl,
                        (unsigned long long)h->g[gi].snap_bytes,
                        (unsigned long long)((now_us_() - t0) / 1000));
                pthread_mutex_unlock(&h->snap_mu);
                host_snap_unlink_other(h, group, path);
                continue;
            }
        }
        pthread_mutex_unlock(&h->snap_mu);
    }
}

/* One queued InstallSnapshot. The local image is a pinned view, so the
 * scan does not hold the LSM lock. import_block covers only this group. */
static void host_snap_import_pass(struct efs_raft_host *h)
{
    int gi;

    for (gi = 0; gi < HOST_NGROUPS && h->gc_running; gi++) {
        char path[EFS_MAX_PATH], local[EFS_MAX_PATH];
        struct efs_kv_lsm_view *view = NULL;
        uint64_t incl, gen, t0;
        uint8_t group;
        int rc;

        pthread_mutex_lock(&h->snap_mu);
        if (h->g[gi].import_state != SNAP_IMP_QUEUED ||
            now_us_() < h->g[gi].import_retry_us) {
            pthread_mutex_unlock(&h->snap_mu);
            continue;
        }
        h->g[gi].import_state = SNAP_IMP_RUN;
        gen = h->g[gi].import_gen;
        incl = h->g[gi].import_incl;
        group = h->g[gi].group;
        snprintf(path, sizeof(path), "%s", h->g[gi].import_path);
        pthread_mutex_unlock(&h->snap_mu);
        __atomic_store_n(&h->g[gi].import_block, 1, __ATOMIC_RELEASE);
        t0 = now_us_();
        fprintf(stderr, "raft-snap: import start group=%u incl=%llu\n",
                group, (unsigned long long)incl);
        snprintf(local, sizeof(local), "%s/snap-%u-import-local.kvx",
                 h->mdraft, group);
        rc = efs_kv_lsm_flush(h->kv);
        fprintf(stderr, "raft-snap: import flush group=%u rc=%d ms=%llu\n",
                group, rc, (unsigned long long)((now_us_() - t0) / 1000));
        if (rc == EFS_ERR_BUSY) {
            /* L0 is at the cap. Leave the .part queued and try again.
             * Failing the snapshot here made the leader resend immediately. */
            pthread_mutex_lock(&h->snap_mu);
            if (h->g[gi].import_gen == gen &&
                h->g[gi].import_state == SNAP_IMP_RUN) {
                h->g[gi].import_state = SNAP_IMP_QUEUED;
                h->g[gi].import_retry_us = now_us_() + 1000000ull;
            }
            pthread_mutex_unlock(&h->snap_mu);
            __atomic_store_n(&h->g[gi].import_block, 0, __ATOMIC_RELEASE);
            continue;
        }
        if (rc == EFS_OK)
            rc = efs_kv_lsm_view_pin(h->kv, &view);
        if (rc == EFS_OK) {
            rc = efs_kv_lsm_view_export(view, group, local);
            efs_kv_lsm_view_unpin(view);
        }
        fprintf(stderr, "raft-snap: import export group=%u rc=%d ms=%llu\n",
                group, rc, (unsigned long long)((now_us_() - t0) / 1000));
        if (rc == EFS_OK) {
            struct efs_kv_item *diff = NULL;
            uint8_t *ha = NULL, *hb = NULL;
            uint32_t ni = 0;

            rc = efs_kv_group_import_prepare(h->kv, group, path, local,
                                             &diff, &ni, &ha, &hb);
            fprintf(stderr, "raft-snap: import diff group=%u rc=%d n=%u "
                    "ms=%llu\n", group, rc, ni,
                    (unsigned long long)((now_us_() - t0) / 1000));
            if (rc == EFS_OK) {
                pthread_mutex_lock(&h->snap_mu);
                if (h->g[gi].import_gen == gen &&
                    h->g[gi].import_state == SNAP_IMP_RUN) {
                    snap_import_free_diff(&h->g[gi]);
                    h->g[gi].import_diff = diff;
                    h->g[gi].import_ni = ni;
                    h->g[gi].import_at = 0;
                    h->g[gi].import_hold_a = ha;
                    h->g[gi].import_hold_b = hb;
                    h->g[gi].import_rc = EFS_OK;
                    h->g[gi].import_state = SNAP_IMP_DONE;
                    diff = NULL;
                    ha = NULL;
                    hb = NULL;
                }
                pthread_mutex_unlock(&h->snap_mu);
                free(diff);
                free(ha);
                free(hb);
                unlink(local);
                continue;
            }
        }
        unlink(local);
        fprintf(stderr, "raft-snap: import end group=%u incl=%llu rc=%d "
                "ms=%llu\n", group, (unsigned long long)incl, rc,
                (unsigned long long)((now_us_() - t0) / 1000));
        pthread_mutex_lock(&h->snap_mu);
        if (h->g[gi].import_gen == gen &&
            h->g[gi].import_state == SNAP_IMP_RUN) {
            h->g[gi].import_rc = rc;
            h->g[gi].import_state =
                rc == EFS_OK ? SNAP_IMP_DONE : SNAP_IMP_FAIL;
            /* import_block stays until the pump observes DONE/FAIL. */
            pthread_mutex_unlock(&h->snap_mu);
        } else {
            pthread_mutex_unlock(&h->snap_mu);
            __atomic_store_n(&h->g[gi].import_block, 0, __ATOMIC_RELEASE);
        }
    }
}

static void *host_gc_thread(void *arg)
{
    struct efs_raft_host *h = arg;
    int g;

    while (h->gc_running) {
        uint64_t t0, t_reap = 0, t_frag = 0, t_spread, t_rec, t_all;
        struct efs_kv_scan_stats sc_frag, sc_all;

        __atomic_store_n(&h->s->gc_pass_start_us, efs_iostats_now_us(), __ATOMIC_RELAXED);
        __atomic_store_n(&h->s->gc_stage, 1, __ATOMIC_RELAXED);
        host_snap_export_pass(h);
        __atomic_store_n(&h->s->gc_stage, 2, __ATOMIC_RELAXED);
        host_snap_import_pass(h);
        memset(&sc_frag, 0, sizeof(sc_frag));
        efs_kv_lsm_scan_stats(NULL, 1);
        t0 = now_us_();
        for (g = 0; g < HOST_NGROUPS && h->gc_running; g++) {
            uint32_t anchor;
            int lead = 0;
            uint64_t ta;

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
            if (!lead) {
                h->orphan_discovery_done[g] = 0;
                h->orphan_discovery_len[g] = 0;
                __atomic_fetch_and(&h->s->gc_sampled_mask, ~(1u << g), __ATOMIC_RELAXED);
                continue;
            }
            /* Group 0 owns the odd shards (anchor 1), group 2 the even
             * ones (anchor 2) — efs_kv_anchor_shard's parity rule. */
            anchor = (h->g[g].group == 0) ? 1u : 2u;
            ta = now_us_();
            __atomic_store_n(&h->s->gc_group, h->g[g].group, __ATOMIC_RELAXED);
            __atomic_store_n(&h->s->gc_stage, 3, __ATOMIC_RELAXED);
            host_gc_orphan_pass(h, h->g[g].group, anchor);
            host_gc_reap_pass(h, h->g[g].group, anchor);
            t_reap += now_us_() - ta;
            ta = now_us_();
            {
                struct efs_kv_scan_stats b, a;

                efs_kv_lsm_scan_stats(&b, 0);
                __atomic_store_n(&h->s->gc_stage, 4, __ATOMIC_RELAXED);
                host_gc_frag_pass(h, h->g[g].group, anchor);
                efs_kv_lsm_scan_stats(&a, 0);
                sc_frag.scans += a.scans - b.scans;
                sc_frag.segs += a.segs - b.segs;
                sc_frag.keys += a.keys - b.keys;
                sc_frag.emitted += a.emitted - b.emitted;
                sc_frag.tombstones += a.tombstones - b.tombstones;
            }
            t_frag += now_us_() - ta;
        }
        t_spread = now_us_();
        __atomic_store_n(&h->s->gc_stage, 5, __ATOMIC_RELAXED);
        host_dir_spread_pass(h);
        t_spread = now_us_() - t_spread;
        t_rec = now_us_();
        __atomic_store_n(&h->s->gc_stage, 6, __ATOMIC_RELAXED);
        host_txn_recover_pass(h);
        t_rec = now_us_() - t_rec;
        t_all = now_us_() - t0;
        efs_kv_lsm_scan_stats(&sc_all, 1);
        /* A pass that holds the KV for a while is a periodic stall on
         * every metadata op (Sep 30: du/find on fstor007 stalled 14 ms
         * once per 1.014 s). Name the slow part. W44 step a: say what
         * the frag pass's scans visited — segment iterators opened,
         * merged keys seen, PUTs handed to the callback, tombstones
         * consumed — and the same for the whole pass. An empty 205 ms
         * frag pass with `fkeys=0 fsegs=55` is the L0 file count (D26);
         * one with `ftomb` in the thousands is tombstones under the GC
         * prefix. `EFS_GC_DBG` prints the line on every pass. */
        if (t_all > 5000 || env_on("EFS_GC_DBG"))
            fprintf(stderr, "raft-host: gc-pass ms=%llu reap=%llu frag=%llu "
                    "spread=%llu recover=%llu fscans=%llu fsegs=%llu "
                    "fkeys=%llu femit=%llu ftomb=%llu scans=%llu segs=%llu "
                    "keys=%llu emit=%llu tomb=%llu\n",
                    (unsigned long long)(t_all / 1000),
                    (unsigned long long)(t_reap / 1000),
                    (unsigned long long)(t_frag / 1000),
                    (unsigned long long)(t_spread / 1000),
                    (unsigned long long)(t_rec / 1000),
                    (unsigned long long)sc_frag.scans,
                    (unsigned long long)sc_frag.segs,
                    (unsigned long long)sc_frag.keys,
                    (unsigned long long)sc_frag.emitted,
                    (unsigned long long)sc_frag.tombstones,
                    (unsigned long long)sc_all.scans,
                    (unsigned long long)sc_all.segs,
                    (unsigned long long)sc_all.keys,
                    (unsigned long long)sc_all.emitted,
                    (unsigned long long)sc_all.tombstones);
        __atomic_store_n(&h->s->gc_last_pass_us, efs_iostats_now_us(), __ATOMIC_RELAXED);
        __atomic_fetch_add(&h->s->gc_passes, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&h->s->gc_stage, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&h->s->gc_pass_start_us, 0, __ATOMIC_RELAXED);
        /* ~1s between passes, in 20 ms slices so shutdown is prompt.
         * A snapshot export queued by the pump starts on the next slice. */
        for (g = 0; g < GC_LOOP_MS / 20 && h->gc_running; g++) {
            host_snap_export_pass(h);
            host_snap_import_pass(h);
            usleep(20 * 1000);
        }
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

    /* The Raft+KV engine is the only metadata path, so the host always
     * starts — there is no env gate. */
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
    pthread_mutex_init(&h->snap_mu, NULL);
    pthread_mutex_init(&h->wait_mu, NULL);
    pthread_mutex_init(&h->inbox_mu, NULL);
    pthread_mutex_init(&h->outbox_mu, NULL);
    pthread_mutex_init(&h->rec_mu, NULL);
    pthread_mutex_init(&h->fin_mu, NULL);
    pthread_cond_init(&h->fin_cv, NULL);
    h->fin = calloc(HOST_FIN_MAX, sizeof(*h->fin));
    /* A fresh process knows nothing about pending txn records: walk every
     * shard once (throttled by the GC pass), then only marked ones. */
    host_rec_mark_all(h);
    pthread_mutex_init(&h->cv_mu, NULL);
    pthread_cond_init(&h->applied_cv, NULL);
    {
        uint32_t k;
        for (k = 0; k < HOST_WAITERS; k++)
            pthread_cond_init(&h->waiters[k].cv, NULL);
    }
    {
        int i;
        for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
            h->tx[i].h = h;
            h->tx[i].peer = i;
            pthread_cond_init(&h->tx[i].cv, NULL);
        }
    }
    h->pump_efd = eventfd(0, EFD_NONBLOCK);

    snprintf(dir, sizeof(dir), "%s/mdraft",
             s->meta_storage[0] ? s->meta_storage : s->storage_path);
    snprintf(h->mdraft, sizeof(h->mdraft), "%s", dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "raft-host: mkdir %s: %s\n", dir, strerror(errno));
        free(h);
        return EFS_ERR_IO;
    }
    host_snap_drop_parts(h);
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
    /* Txn finisher (see host_fin_one). Without it recovery still resolves
     * an abandoned commit, only 5 s later. */
    h->fin_running = h->fin != NULL;
    if (h->fin_running &&
        efsd_pthread_create(&h->fin_tid, host_fin_thread, h) != 0) {
        fprintf(stderr, "raft-host: txn finisher thread failed; "
                "recovery covers abandoned commits\n");
        h->fin_running = 0;
    } else if (h->fin_running) {
        h->fin_started = 1;
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
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++)
        pthread_cond_broadcast(&h->tx[i].cv);
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
    pthread_mutex_lock(&h->fin_mu);
    h->fin_running = 0;
    pthread_cond_broadcast(&h->fin_cv);
    pthread_mutex_unlock(&h->fin_mu);
    if (h->fin_started)
        pthread_join(h->fin_tid, NULL);
    free(h->fin);
    h->fin = NULL;
    h->running = 0;
    /* Wake the pump so shutdown does not wait out a 5 ms tick. */
    host_pump_kick(h);
    pthread_mutex_lock(&h->wait_mu);
    for (w = h->wait_head; w; w = w->next)
        pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&h->wait_mu);
    if (h->started)
        pthread_join(h->tid, NULL);
    for (i = 0; i < HOST_NGROUPS; i++) {
        if (h->g[i].snap_view)
            efs_kv_lsm_view_unpin(h->g[i].snap_view);
        h->g[i].snap_view = NULL;
        if (h->g[i].snap_fd >= 0)
            close(h->g[i].snap_fd);
        h->g[i].snap_fd = -1;
        if (h->g[i].part_fd >= 0) {
            close(h->g[i].part_fd);
            unlink(h->g[i].part_path);
        }
        h->g[i].part_fd = -1;
        if (h->g[i].applied_fd >= 0)
            close(h->g[i].applied_fd);
        h->g[i].applied_fd = -1;
    }
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
    pthread_mutex_destroy(&h->snap_mu);
    pthread_mutex_destroy(&h->wait_mu);
    pthread_mutex_destroy(&h->inbox_mu);
    pthread_mutex_destroy(&h->outbox_mu);
    pthread_mutex_destroy(&h->rec_mu);
    pthread_cond_destroy(&h->applied_cv);
    {
        uint32_t k;
        for (k = 0; k < HOST_WAITERS; k++)
            pthread_cond_destroy(&h->waiters[k].cv);
    }
    pthread_mutex_destroy(&h->cv_mu);
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++)
        pthread_cond_destroy(&h->tx[i].cv);
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
        uint64_t n;

        pthread_mutex_unlock(&h->inbox_mu);
        n = __atomic_add_fetch(&h->inbox_drop, 1, __ATOMIC_RELAXED);
        if (n <= 10)
            fprintf(stderr,
                    "raft-host: inbox full, dropped frame type=%u group=%u "
                    "from=%d len=%u (drop #%llu)\n",
                    (unsigned)copy[0], (unsigned)copy[1],
                    (int)(int32_t)(((uint32_t)copy[4] << 24) |
                                   ((uint32_t)copy[5] << 16) |
                                   ((uint32_t)copy[6] << 8) |
                                   (uint32_t)copy[7]),
                    plen, (unsigned long long)n);
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
    uint8_t scmd[13];
    int hint = -1;
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
    /* Wait for the group-0 commit, then replicate the salt to the
     * even-shard group (EFS_MD_CMD_SALT anchored on shard 2): every MKDIR
     * scatters via efs_kv_mkdir_shard(parent, name, salt) on EVERY group,
     * and a node hosting only group 2 never applies MKFS — without this
     * record its salt read returned 0 and its placement diverged (apply
     * PROTO skip -> missing rows -> later EIO). mkfs only returns OK once
     * both groups carry the salt. */
    rc = host_propose_wait(h, EFS_RAFT_GROUP_SHARD, cmd, 17, &hint);
    if (rc == EFS_OK) {
        /* W37: the SALT step carries the salt the COMMITTED MKFS recorded,
         * never this node's own. `efs_meta_apply_mkfs` on an existing root
         * is a no-op that keeps the first salt, so a retry of mkfs from a
         * second node used to commit MKFS "fine" and then replicate ITS
         * salt to group 2 — `efs_meta_apply_salt_record` PROTO on every
         * group-2 apply, only a wipe recovers (Oct 1). Read the record
         * back (root shard's export key; group 2's anchor if this node
         * holds only that); a node that holds neither cannot know the
         * salt and answers BUSY — retry on a group-0 node. */
        uint64_t committed = 0;
        int src = efs_meta_apply_export_salt(h->kv, &committed);
        if (src != EFS_OK) {
            fprintf(stderr, "raft-host: raft-mkfs: MKFS committed but this "
                    "node holds no salt record (rc=%d); retry on a group-0 "
                    "node\n", src);
            rc = EFS_ERR_BUSY;
        } else if (committed != h->salt) {
            fprintf(stderr, "raft-host: raft-mkfs: table exists with salt %llu; "
                    "own salt %llu not used (mkfs once, on one node)\n",
                    (unsigned long long)committed, (unsigned long long)h->salt);
            h->salt = committed;
        }
    }
    if (rc == EFS_OK) {
        scmd[0] = EFS_MD_CMD_SALT;
        wr32be(scmd + 1, efs_kv_anchor_shard(2));
        wr64be(scmd + 5, h->salt);
        rc = host_propose_wait(h, EFS_RAFT_GROUP_SHARD2, scmd, 13, &hint);
    }
    out->salt = h->salt;
    pthread_mutex_lock(&h->mu);
    r = group_raft(h, EFS_RAFT_GROUP_SHARD);
    if (r)
        out->index = efs_raft_applied(r);
    pthread_mutex_unlock(&h->mu);
    out->rc = rc;
    if (hint >= 0)
        out->leader_hint = hint;
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

/* ESTABLISH must follow authoritative ACTIVE + REGISTER on the session
 * owner. This is a one-time admission check, not a global lookup per write.
 * If fencing starts after this read, REGISTER is already in its frozen set;
 * ordered FENCE_LOC then rejects a delayed old-epoch ESTABLISH on the lane. */
static int host_session_establish_admit(struct efs_raft_host *h,
    const uint8_t *cmd, uint32_t clen, uint8_t group, int *hint)
{
    if (clen != HOST_SESS_REG_LEN) return EFS_ERR_INVAL;
    uint32_t epoch=rd32be(cmd+18), shard=rd32be(cmd+22);
    if (!epoch || shard>=EFS_SESSION_BITS || group!=efs_raft_shard_group(shard))
        return EFS_ERR_INVAL;
    uint8_t query[HOST_SESS_HDR_LEN+4];
    memcpy(query,cmd,HOST_SESS_HDR_LEN);query[1]=HOST_SESS_GET;
    wr32be(query+18,shard);
    uint8_t owner=efs_raft_shard_group(efs_kv_session_shard(cmd+2));
    uint64_t salt=0;int rc;
    if (host_hosts(h,owner)) rc=host_session_get(h,query,sizeof(query),owner,hint,&salt);
    else {
        struct efs_msg_raft_mkfs_reply reply;
        memset(&reply,0,sizeof(reply));
        rc=host_remote_cmd(h,owner,query,sizeof(query),&reply,-1);
        if (rc==EFS_OK) salt=reply.salt;
    }
    if (rc!=EFS_OK) return rc;
    uint32_t current=(uint32_t)salt;
    uint8_t state=(uint8_t)(salt>>32);
    if (state==EFS_SESSION_FENCING) return epoch==current ? EFS_ERR_BUSY : EFS_ERR_STALE;
    if (state!=EFS_SESSION_ACTIVE) return EFS_ERR_PROTO;
    if (epoch!=current) return EFS_ERR_STALE;
    return (salt & (1ull<<40)) ? EFS_OK : EFS_ERR_BUSY;
}

static int host_dir_migrate(struct efs_raft_host *h, efs_ino_t dir,
                            const uint8_t *cmd, uint32_t clen, int *hint);

/* Payload: group byte, then command bytes (empty command = ReadIndex).
 * A hosted replica proposes (or ReadIndexes) even when it is not the
 * leader; an unhosted node forwards a non-empty command to a voter.
 * Session GET (sub=0) is a ReadIndex, not a
 * log command: salt carries epoch + state + touched-bit (10.5c-35a). */
void server_raft_host_submit(const uint8_t *payload, uint32_t plen,
                             struct efs_msg_raft_mkfs_reply *out)
{
    struct efs_raft_host *h = g_host;
    uint8_t group;
    const uint8_t *cmd;
    uint32_t clen;
    uint64_t idx = 0, term = 0;
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
    if (clen >= HOST_CFG_LEN && cmd[0] == EFS_MD_CMD_CFG) {
        rc = host_cfg(h, group, cmd, clen, &hint);
        {
            struct host_view v;
            host_view_get(h, group, &v);
            idx = v.applied;
        }
        out->rc = rc;
        out->index = idx;
        if (hint >= 0)
            out->leader_hint = hint;
        return;
    }
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
        rc = host_session_get(h, cmd, clen, group, &hint, &salt);
        {
            struct host_view v;
            host_view_get(h, group, &v);
            idx = v.applied;
        }
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
    /* A forwarded command is answered here only by the leader. A hosted
     * follower used to run host_read_index/host_propose, which forward
     * again — under a leaderless group every hop parked a conn thread and
     * a peer-pool slot on the next node's recv (56 + 48 such threads on
     * fcstor003, Sep 21) and the chain kept the pool full. The caller's
     * host_remote_cmd retries the hinted leader itself. DIR_MIGRATE is
     * exempt: it is bounced to a dual-host precisely because the pg
     * leader may not host the dest group; its own reads/proposes each
     * forward one hop to a leader that answers. */
    {
        struct host_view v;
        host_view_get(h, group, &v);
        if ((!v.has || v.role != EFS_RAFT_LEADER) &&
            !(clen >= HOST_DIR_LEN && cmd[0] == EFS_MD_CMD_DIR &&
              cmd[1] == EFS_MD_DIR_MIGRATE)) {
            out->rc = EFS_ERR_NOT_PRIMARY;
            if (v.has && v.leader >= 0 && v.leader != h->raft_id)
                out->leader_hint = v.leader;
            return;
        }
    }
    if (clen >= 2 && cmd[0] == EFS_MD_CMD_SESSION &&
        cmd[1] == EFS_MD_SESS_ESTABLISH) {
        rc = host_session_establish_admit(h, cmd, clen, group, &hint);
        if (rc != EFS_OK) { out->rc = rc; return; }
    }
    if (clen == 0) {
        rc = host_read_index(h, group, &hint);
        {
            struct host_view v;
            host_view_get(h, group, &v);
            idx = v.applied;
        }
    } else if (clen >= HOST_DIR_LEN && cmd[0] == EFS_MD_CMD_DIR &&
               cmd[1] == EFS_MD_DIR_MIGRATE) {
        rc = host_dir_migrate(h, rd64be(cmd + 2), cmd, clen, &hint);
        {
            struct host_view v;
            host_view_get(h, group, &v);
            idx = v.applied;
        }
    } else if (clen >= 1 && cmd[0] == EFS_MD_CMD_PUBLISH) {
        /* Same as the local report path: propose, return the index, let
         * the reporter host_wait_applied the last idx. host_propose_wait
         * here was one Raft round per forwarded batch (~16 × ~150 ms on
         * the group this dual-host does not lead). */
        rc = host_propose(h, group, cmd, clen, &idx, &term, &hint);
    } else {
        /* Return THIS command's index, never the live applied index. The
         * forwarding follower waits for out->index and reads its own
         * apply-result ring at that slot (host_propose_wait_ex → arc_rc /
         * arc_extra). Under concurrent traffic applied had already moved
         * past the command by the time it was read here, so the follower
         * read a stranger's slot: rc OK, extra 0. For APPEND_RSV that
         * replied start=0/size=len while the reservation itself landed at
         * the real EOF — the O_APPEND write went to offset 0 and left a
         * len-byte hole at EOF (concurrent_appends 190–198/200 lines, one
         * 2-byte gap per forwarded reserve). It also let a forwarded txn
         * PREP's BUSY/STALE verdict read as OK. */
        rc = host_propose_wait_idx(h, group, cmd, clen, &hint, &idx);
    }
    out->rc = rc;
    out->index = idx;
    out->term = term; /* 0 unless the PUBLISH branch proposed without waiting */
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
        gs->joint = 0;
        if (h->g[i].hosted && h->g[i].r) {
            gs->role = (uint8_t)efs_raft_role(h->g[i].r);
            gs->leader = efs_raft_leader(h->g[i].r);
            gs->term = efs_raft_term(h->g[i].r);
            gs->commit_index = efs_raft_commit(h->g[i].r);
            gs->applied_index = efs_raft_applied(h->g[i].r);
            gs->voters = efs_raft_voters(h->g[i].r);
            gs->joint = efs_raft_joint(h->g[i].r) ? 1 : 0;
        }
    }
    pthread_mutex_unlock(&h->mu);
    src = efs_meta_apply_export_salt(h->kv, &salt);
    if (src == EFS_OK) {
        struct efs_meta_row row;
        /* Salt is reported whenever a record exists — a node hosting only
         * the even group has the SALT record but no root row, and gating
         * the salt on the root made such a node falsely report salt=0. */
        out->export_salt = salt;
        if (efs_meta_apply_get_inode(h->kv, EFS_ROOT_INO, &row) == EFS_OK)
            out->kv_has_root = 1;
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
    int prof = read_prof_on();
    uint64_t p0 = prof ? now_us_() : 0, p1 = 0;

    {
        uint8_t ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
        if (!host_hosts(h, ig)) {
            if (prof)
                __atomic_fetch_add(&g_read_prof.ga_fwd, 1, __ATOMIC_RELAXED);
            host_fwd_getattr(h, ino, out, &ig, 1);
            return;
        }
    }
    rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        uint8_t need[2];

        host_need_both(need);
        if (prof)
            __atomic_fetch_add(&g_read_prof.ga_fwd, 1, __ATOMIC_RELAXED);
        host_fwd_getattr(h, ino, out, need, 2);
        return;
    }
    if (prof)
        p1 = now_us_();
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
    if (prof) {
        uint64_t p2 = now_us_();

        read_prof_add(&g_read_prof.ga_total, p2 - p0);
        read_prof_add(&g_read_prof.ga_lanes, p1 - p0);
        read_prof_add(&g_read_prof.ga_getattr, p2 - p1);
        read_prof_tick();
    }
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
    rc = host_read_index(h, ig, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
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
 * client re-issues (the queue rebuilds there). wait_mu is never held
 * while sleeping or across a Raft round. */

#define HOST_LOCK_WAIT_MS 50

static int host_is_leader(struct efs_raft_host *h, uint8_t group)
{
    struct host_view v;

    host_view_get(h, group, &v);
    return v.has && v.role == EFS_RAFT_LEADER;
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
    rc = host_read_index(h, ig, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
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
    int prof = read_prof_on();
    uint64_t p0 = prof ? now_us_() : 0, p1 = 0, p2 = 0, p3 = 0;

    psh = efs_kv_inode_shard(parent);
    pg = efs_raft_shard_group(psh);
    if (!host_hosts(h, pg)) {
        uint8_t need[2];
        need[0] = pg;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        if (prof)
            __atomic_fetch_add(&g_read_prof.lk_fwd, 1, __ATOMIC_RELAXED);
        host_fwd_lookup(h, parent, name, out, need, 2);
        return;
    }
    rc = host_read_index(h, pg, &hint);
    if (prof)
        p1 = now_us_();
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
                host_fwd_lookup(h, parent, name, out, need, 2);
                return;
            }
            rc = host_read_index(h, hg, &hh);
            if (rc != EFS_OK)
                hint = hh;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup_tx(h->kv, parent, name, host_txn_coord, h, &dent);
    if (prof)
        p2 = now_us_();
    if (rc == EFS_OK) {
        uint8_t cg = efs_raft_shard_group(efs_kv_inode_shard(dent.ino));
        if (!host_hosts(h, cg)) {
            /* The dentry was read here under pg's ReadIndex; only the
             * child's row is on a group this host does not hold. Ask
             * that group for a GETATTR (its leader when known) instead
             * of re-running the whole LOOKUP on a dual host, which paid
             * two follower ReadIndex round trips (Spark Oct 2: 1.5 ms
             * per forwarded lookup against 20 us served). Same two
             * independent ReadIndexes as before, one fewer hop. The
             * child's lanes may still send the peer to a dual host. */
            if (prof)
                __atomic_fetch_add(&g_read_prof.lk_fwd, 1, __ATOMIC_RELAXED);
            host_fwd_getattr(h, dent.ino, out, &cg, 1);
            if (out->status == EFS_INODE_RPC_OK) {
                out->inode.parent = parent;
                strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
            }
            return;
        }
        rc = host_read_inode_lanes(h, dent.ino, &hint);
        if (rc == EFS_ERR_NOT_PRIMARY) {
            uint8_t need[2];

            host_need_both(need);
            if (prof)
                __atomic_fetch_add(&g_read_prof.lk_fwd, 1, __ATOMIC_RELAXED);
            host_fwd_lookup(h, parent, name, out, need, 2);
            return;
        }
    }
    if (prof)
        p3 = now_us_();
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, dent.ino, host_txn_coord, h, &st);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
    if (prof) {
        uint64_t p4 = now_us_();

        read_prof_add(&g_read_prof.lk_total, p4 - p0);
        read_prof_add(&g_read_prof.lk_ri_parent, p1 - p0);
        read_prof_add(&g_read_prof.lk_kv, p2 - p1);
        read_prof_add(&g_read_prof.lk_lanes, p3 - p2);
        read_prof_add(&g_read_prof.lk_getattr, p4 - p3);
        read_prof_tick();
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
                                  const struct efs_opid_req *q, int *hint);

/* I16 leader-side probe: the op's window lives on `shard` (the dentry
 * shard of the named entry), which this host must hold. 1 = replay, reply
 * filled from the recorded verdict; 0 = new; <0 = KV error. */
static int host_opid_replay(struct efs_raft_host *h, uint32_t shard,
                            const struct efs_opid_req *q,
                            struct efs_opid_reply *rep)
{
    int hit;

    if (!q)
        return 0;
    hit = efs_meta_apply_opid_probe(h->kv, shard, &q->id, rep);
    if (hit > 0)
        __atomic_fetch_add(&h->obs_opid_replay, 1, __ATOMIC_RELAXED);
    return hit;
}

/* Reply for a replayed directory op that created or named an inode: the
 * recorded ino's current row (stat) or, if it is already gone again, the
 * bare OK the first attempt got. */
static void host_opid_reply_ino(struct efs_raft_host *h, efs_ino_t parent,
                                const char *name, const struct efs_opid_reply *rep,
                                struct efs_msg_inode_reply *out)
{
    struct efs_meta_row row;
    struct efs_meta_stat st;

    set_inode_rc(out, rep->rc, -1);
    if (rep->rc != EFS_OK || !rep->ino) {
        /* An acked-and-reclaimed stub (ino 0) should never answer a live
         * retry: the client acks only below its lowest in-flight seq. */
        fprintf(stderr, "raft-host: opid-replay parent=%llu name=%s seq=%llu "
                "rc=%d ino=%llu (stub)\n", (unsigned long long)parent, name,
                (unsigned long long)rep->seq, rep->rc,
                (unsigned long long)rep->ino);
        return;
    }
    if (efs_meta_apply_get_inode(h->kv, rep->ino, &row) == EFS_OK) {
        host_stat_from_row(&row, &st);
        stat_to_inode(&st, &out->inode);
    } else {
        out->inode.ino = rep->ino;
        fprintf(stderr, "raft-host: opid-replay parent=%llu name=%s seq=%llu "
                "ino=%llu row gone\n", (unsigned long long)parent, name,
                (unsigned long long)rep->seq, (unsigned long long)rep->ino);
    }
    out->inode.parent = parent;
    strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
}

void server_raft_host_create(efs_ino_t parent, const char *name, uint32_t mode,
                             uint32_t uid, uint32_t gid, uint32_t flags,
                             uint64_t owner, const struct efs_opid_req *q,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_meta_attrs at;
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t clen = 0;
    uint64_t idx = 0, term = 0;
    uint32_t dsh = 0;
    uint8_t pg, dg = 0;
    int hint = -1;
    int rc;
    int hashed = 0;
    uint64_t hold = (flags & EFS_CREATE_F_HOLD) ? owner : 0;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if ((mode & S_IFMT) == 0)
        mode |= S_IFREG;
    if (S_ISDIR(mode)) {
        server_raft_host_mkdir(parent, name, mode, uid, gid, q, out);
        return;
    }
    at.uid = uid;
    at.gid = gid;
    at.now = now_ns();
    rc = pack_create_cmd(cmd, &clen, parent, mode, name, &at, hold, 0, 0, q);
    if (rc != EFS_OK) {
        set_inode_rc(out, rc, -1);
        return;
    }
    pg = efs_raft_shard_group(efs_kv_inode_shard(parent));
    if (!host_hosts(h, pg)) {
        uint8_t need[2];
        need[0] = pg;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_create(h, parent, name, mode, uid, gid, flags, owner, q, out,
                        need, 2);
        return;
    }
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
            host_fwd_create(h, parent, name, mode, uid, gid, flags, owner, q,
                            out, need, nn);
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
        struct efs_opid_reply rep;
        int hit = host_opid_replay(h, dsh, q, &rep);

        if (hit < 0) {
            rc = hit;
        } else if (hit) {
            host_opid_reply_ino(h, parent, name, &rep, out);
            return;
        }
    }
    if (rc == EFS_OK) {
        rc = efs_meta_apply_lookup_tx(h->kv, parent, name, host_txn_coord, h, &dent);
        if (rc == EFS_OK) {
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
        pg != dg) {
        hashed = 1;
        rc = host_hashed_create_txn(h, parent, name, mode, &at, &prow, dsh, q,
                                    &hint);
    } else if (rc == EFS_OK) {
        efs_ino_t next = 0;

        rc = efs_meta_apply_peek_alloc(h->kv, dsh, &next);
        if (rc == EFS_OK)
            rc = pack_create_cmd(cmd, &clen, parent, mode, name, &at, hold,
                                 next, prow.layout, q);
        if (rc == EFS_OK)
            rc = host_propose(h, dg, cmd, clen, &idx, &term, &hint);
        if (rc == EFS_OK)
            rc = host_wait_verdict(h, dg, idx, term, &hint);
    }
    if (rc == EFS_OK) {
        int lrc = efs_meta_apply_lookup_tx(h->kv, parent, name, host_txn_coord,
                                           h, &dent);

        /* The command committed and the name is not there. That is a
         * committed create answered as failure (the I16 class); the
         * client's create then returns EIO. */
        if (lrc == EFS_ERR_NOT_FOUND)
            fprintf(stderr, "raft-host: create committed but lookup missed "
                    "parent=%llu name=%s idx=%llu term=%llu\n",
                    (unsigned long long)parent, name,
                    (unsigned long long)idx, (unsigned long long)term);
        rc = lrc;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, dent.ino, &prow);
    if (rc == EFS_OK && hashed && hold) {
        uint8_t lcmd[HOST_CMD_MAX], uuid[EFS_OPID_UUID_LEN];
        uint32_t llen = 0;
        uint8_t ig = efs_raft_shard_group(efs_kv_inode_shard(dent.ino));

        host_hold_uuid(hold, uuid);
        pack_lease_cmd(lcmd, &llen, 1, dent.ino, prow.generation, uuid, 1);
        rc = host_propose_wait(h, ig, lcmd, llen, &hint);
    }
    if (rc == EFS_OK)
        host_stat_from_row(&prow, &st);
    if (dirop_fail_on(rc))
        fprintf(stderr, "raft-host: create parent=%llu name=%s rc=%d hint=%d "
                "dsh=%u hashed=%d\n",
                (unsigned long long)parent, name, rc, hint, dsh, hashed);
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
    if (p->n >= EFS_TXN_NAMESPACE_MAX_PART)
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
 * BUSY (EFS_TXN_NAMESPACE_MAX_PART). */
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
        if (*ng >= EFS_TXN_NAMESPACE_MAX_PART)
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

struct host_idx_ref {
    uint8_t group;
    uint64_t idx;
    uint64_t term; /* 0 = forwarded (leader matched its own term) */
};

static int host_wait_refs(struct efs_raft_host *h, struct host_idx_ref *refs,
                          int n, int *hint)
{
    int i, j, rc, one;

    if (n <= 0)
        return EFS_OK;
    for (i = 0; i < n; i++) {
        uint64_t mx = refs[i].idx;
        int seen = 0;

        for (j = 0; j < i; j++) {
            if (refs[j].group == refs[i].group) {
                seen = 1;
                break;
            }
        }
        if (seen)
            continue;
        for (j = i + 1; j < n; j++) {
            if (refs[j].group == refs[i].group && refs[j].idx > mx)
                mx = refs[j].idx;
        }
        rc = host_wait_applied(h, refs[i].group, mx, hint);
        if (rc != EFS_OK)
            return rc;
    }
    rc = EFS_OK;
    for (i = 0; i < n; i++) {
        pthread_mutex_lock(&h->mu);
        one = host_apply_rc_locked(h, refs[i].group, refs[i].idx,
                                   refs[i].term);
        pthread_mutex_unlock(&h->mu);
        if (one != EFS_OK && rc == EFS_OK)
            rc = one;
    }
    return rc;
}

/* DECIDE COMMIT at the coordinator, then RESOLVE COMMIT on every part
 * (proposed together, waited together). A wait that runs out after the
 * DECIDE is in the log goes to the finisher; the caller still sees the
 * error (the client retries, and the op-id window answers the retry). */
static int host_txn_commit(struct efs_raft_host *h, const struct efs_txid *t,
                           const struct efs_txn_parts *parts, uint32_t coord,
                           int *hint)
{
    uint8_t cmd[22];
    struct host_idx_ref refs[EFS_TXN_MAX_PART];
    int i, n = 0, rc;

    pack_decide(cmd, t, coord, EFS_TXN_COMMIT);
    rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22, hint);
    if (rc != EFS_OK) {
        /* BUSY = the wait ran out; the entry may well commit. Any other
         * verdict means it was decided differently (recovery ABORT) or
         * never proposed — nothing to finish. */
        if (rc == EFS_ERR_BUSY)
            host_fin_add(h, t, parts, coord);
        return rc;
    }
    for (i = 0; i < parts->n && i < EFS_TXN_MAX_PART; i++) {
        pack_resolve(cmd, t, parts->shard[i], EFS_TXN_COMMIT);
        refs[n].group = efs_raft_shard_group(parts->shard[i]);
        rc = host_propose(h, refs[n].group, cmd, 22, &refs[n].idx,
                          &refs[n].term, hint);
        if (rc != EFS_OK)
            break;
        n++;
    }
    if (rc == EFS_OK)
        rc = host_wait_refs(h, refs, n, hint);
    if (rc != EFS_OK)
        host_fin_add(h, t, parts, coord);
    return rc;
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

static int host_prep_async(struct efs_raft_host *h, uint32_t shard, int kind,
                           const struct efs_txid *t, const struct efs_txn_parts *p,
                           const uint8_t *key, uint32_t klen, uint64_t expected,
                           int op, const uint8_t *val, uint32_t vlen,
                           struct host_idx_ref *ref, int *hint)
{
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t n;

    n = pack_prep(cmd, kind, t, p, key, klen, expected, op, val, vlen);
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    ref->group = efs_raft_shard_group(shard);
    return host_propose(h, ref->group, cmd, n, &ref->idx, &ref->term,
                        hint);
}

/* Commutative parts (§7.2). `ref` NULL = propose and wait (host_prep
 * style); non-NULL = propose only, caller waits with host_wait_refs
 * (host_prep_async style). */
static int host_prep_raw(struct efs_raft_host *h, uint32_t shard, int kind,
                         const struct efs_txid *t, const struct efs_txn_parts *p,
                         const uint8_t *key, uint32_t klen, const uint8_t *pay,
                         uint32_t plen, struct host_idx_ref *ref, int *hint)
{
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t n;

    n = pack_prep_raw(cmd, kind, t, p, key, klen, pay, plen);
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    if (!ref)
        return host_propose_wait(h, efs_raft_shard_group(shard), cmd, n, hint);
    ref->group = efs_raft_shard_group(shard);
    return host_propose(h, ref->group, cmd, n, &ref->idx, &ref->term,
                        hint);
}

/* Namespace rows can change through unversioned log applies. Compare their
 * captured bytes, rather than a version sidecar that those applies do not bump. */
static int host_prep_value(struct efs_raft_host *h, uint32_t shard,
                           const struct efs_txid *t, const struct efs_txn_parts *parts,
                           const uint8_t *key, uint32_t kl,
                           const uint8_t *expected, uint32_t en, int op,
                           const uint8_t *value, uint32_t vn, int *hint)
{
    uint8_t pay[9 + 2 * EFS_META_DENT_BYTES];
    uint32_t pn = 0;
    int rc = efs_txn_encode_excl_value(pay, sizeof(pay), &pn, expected, en,
                                       op, value, vn);
    if (rc != EFS_OK)
        return rc;
    return host_prep_raw(h, shard, EFS_TXN_EXCL_VALUE, t, parts, key, kl,
                          pay, pn, NULL, hint);
}

/* Parent (or moved-dir) inode row: nlink/nents delta, times MAX,
 * used_shards OR, parent SET. Replaces the EXCL full-image PUT that lost
 * concurrent log-path updates and serialized every op in one directory. */
static int host_prep_ino_delta(struct efs_raft_host *h, uint32_t shard,
                               const struct efs_txid *t,
                               const struct efs_txn_parts *p, const uint8_t *key,
                               uint32_t klen, const struct efs_txn_ino_delta *d,
                               struct host_idx_ref *ref, int *hint)
{
    uint8_t pay[EFS_TXN_REDUCE_INO_WIRE];

    efs_txn_encode_ino_delta(pay, d);
    return host_prep_raw(h, shard, EFS_TXN_REDUCE_INO, t, p, key, klen, pay,
                         sizeof(pay), ref, hint);
}

/* dseq emptiness witness +1 (commutes with every other bump; an RMDIR's
 * GUARD on the same witness is BUSY while this is pending and STALE after
 * it folds, which is exactly "the directory is no longer empty"). */
static int host_prep_dseq_bump(struct efs_raft_host *h, uint32_t shard,
                               const struct efs_txid *t,
                               const struct efs_txn_parts *p, const uint8_t *key,
                               uint32_t klen, struct host_idx_ref *ref, int *hint)
{
    uint8_t pay[EFS_TXN_REDUCE_ADD_WIRE];

    efs_txn_encode_add(pay, 1);
    return host_prep_raw(h, shard, EFS_TXN_REDUCE_ADD, t, p, key, klen, pay,
                         sizeof(pay), ref, hint);
}

/* I16 verdict record for a txn directory op: on COMMIT the window on
 * `shard` (the op's dentry shard, already a participant) folds the ack
 * and (OK, ino) for q->id.seq; ABORT leaves the window alone. No-op
 * without an op-id. */
static int host_has_opid(const struct efs_opid_req *q)
{
    return q && efs_opid_req_valid(q);
}

static int host_prep_opid(struct efs_raft_host *h, uint32_t shard,
                          const struct efs_txid *t, const struct efs_txn_parts *p,
                          const struct efs_opid_req *q, efs_ino_t ino,
                          uint64_t extra, struct host_idx_ref *ref, int *hint)
{
    struct efs_opid_reply rep;
    uint8_t key[EFS_KV_KEY_MAX], pay[EFS_TXN_REDUCE_OPID_WIRE];
    uint32_t klen = 0;
    int rc;

    if (!q || !efs_opid_req_valid(q))
        return EFS_OK;
    rc = efs_kv_key_opid(shard, q->id.client_uuid, q->id.session_epoch, key,
                         &klen);
    if (rc != EFS_OK)
        return rc;
    memset(&rep, 0, sizeof(rep));
    rep.seq = q->id.seq;
    rep.rc = EFS_OK;
    rep.ino = ino;
    rep.extra = extra;
    efs_txn_encode_opid(pay, q, &rep);
    return host_prep_raw(h, shard, EFS_TXN_REDUCE_OPID, t, p, key, klen, pay,
                         sizeof(pay), ref, hint);
}

/* HASHED/SPLITTING parent dir-lane stamp: mtime/ctime MAX, seq++, the
 * dir's mtime_gen. Replaces efs_meta_stamp_dir_lane + EXCL PUT. */
static int host_prep_lane_stamp(struct efs_raft_host *h, uint32_t shard,
                                const struct efs_txid *t,
                                const struct efs_txn_parts *p,
                                const struct efs_meta_row *dir, const char *name,
                                uint64_t now, struct host_idx_ref *ref, int *hint)
{
    struct efs_txn_reduce red;
    uint8_t key[EFS_KV_KEY_MAX], pay[EFS_TXN_REDUCE_WIRE];
    uint32_t klen = 0;
    uint8_t lane = efs_kv_dir_lane(name);
    int rc;

    rc = efs_kv_key_lane(efs_kv_lane_shard(dir->ino, lane), dir->ino,
                         dir->generation, lane, key, &klen);
    if (rc != EFS_OK)
        return rc;
    memset(&red, 0, sizeof(red));
    red.max_mtime = now;
    red.max_ctime = now;
    red.mtime_gen = dir->mtime_gen;
    efs_txn_encode_reduce(pay, &red);
    return host_prep_raw(h, shard, EFS_TXN_REDUCE, t, p, key, klen, pay,
                         sizeof(pay), ref, hint);
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

/* D25 handler-side transport. SETATTR SIZE must not select this path until
 * authoritative client masks/cache validation and writer epochs are wired. */
struct host_resize_ctx {
    struct efs_raft_host *host;
    int *hint;
};

static int host_resize_prepare(void *user, const struct efs_txid *t,
                                const struct efs_txn_parts *parts,
                                const struct efs_meta_resize_prepare *q)
{
    struct host_resize_ctx *ctx = user;
    uint8_t key[EFS_KV_KEY_MAX], pay[EFS_META_RESIZE_PREPARE_BYTES];
    uint32_t kl = 0;
    int rc = efs_meta_encode_resize_prepare(q, key, &kl, pay);
    if (rc != EFS_OK)
        return rc;
    uint32_t shard = ((uint32_t)key[0] << 8) | key[1];
    return host_prep_raw(ctx->host, shard, EFS_TXN_CONTENT_RESIZE, t, parts,
                          key, kl, pay, sizeof(pay), NULL, ctx->hint);
}

static int host_resize_decide(void *user, const struct efs_txid *t,
                               const struct efs_txn_parts *parts, int decision)
{
    struct host_resize_ctx *ctx = user;
    uint8_t cmd[22];
    uint32_t coord = efs_txn_coordinator(t, parts);
    pack_decide(cmd, t, coord, decision);
    int rc = host_propose_wait(ctx->host, efs_raft_shard_group(coord), cmd,
                                sizeof(cmd), ctx->hint);
    if (rc == EFS_ERR_BUSY)
        host_fin_add(ctx->host, t, parts, coord);
    return rc;
}

static int host_resize_resolve(void *user, const struct efs_txid *t,
                                const struct efs_txn_parts *parts, int decision)
{
    struct host_resize_ctx *ctx = user;
    struct host_idx_ref refs[EFS_TXN_MAX_PART];
    uint8_t cmd[22];
    int rc = EFS_OK, n = 0;
    for (uint32_t i = 0; i < parts->n; ++i) {
        pack_resolve(cmd, t, parts->shard[i], decision);
        refs[n].group = efs_raft_shard_group(parts->shard[i]);
        rc = host_propose(ctx->host, refs[n].group, cmd, sizeof(cmd),
                           &refs[n].idx, &refs[n].term, ctx->hint);
        if (rc != EFS_OK)
            break;
        ++n;
    }
    if (rc == EFS_OK)
        rc = host_wait_refs(ctx->host, refs, n, ctx->hint);
    if (rc != EFS_OK)
        host_fin_add(ctx->host, t, parts, efs_txn_coordinator(t, parts));
    return rc;
}

static int host_mtime_prepare(void *user, const struct efs_txid *t,
                               const struct efs_txn_parts *parts,
                               const struct efs_meta_mtime_image *image)
{
    struct host_resize_ctx *ctx = user;
    uint8_t pay[9 + 2 * EFS_META_INO_BYTES]; uint32_t pn;
    int rc = efs_txn_encode_excl_value(pay, sizeof(pay), &pn,
        image->expected, image->expected_len, EFS_TXN_PUT, image->value, image->new_len);
    if (rc != EFS_OK) return rc;
    uint32_t shard = ((uint32_t)image->key[0] << 8) | image->key[1];
    return host_prep_raw(ctx->host, shard, EFS_TXN_EXCL_VALUE, t, parts,
                         image->key, image->klen, pay, pn, NULL, ctx->hint);
}

static int host_regular_mtime(struct efs_raft_host *h, efs_ino_t ino,
                               const struct efs_meta_utimens *u, int *hint)
{
    const struct efs_meta_mtime_ops ops = {
        host_mtime_prepare, host_resize_decide, host_resize_resolve};
    struct host_resize_ctx ctx = {h, hint};
    struct efs_meta_mtime_plan *plan = malloc(sizeof(*plan));
    struct efs_txid t;
    if (!plan) return EFS_ERR_NOMEM;
    uint8_t groups[2]; host_need_both(groups);
    int rc = EFS_OK;
    for (unsigned i = 0; i < 2 && rc == EFS_OK; ++i)
        rc = host_read_index(h, groups[i], hint);
    if (rc == EFS_OK)
        rc = efs_meta_capture_mtime(h->kv, ino, now_ns(), u->mask, u->mtime,
                                    u->atime, host_txn_coord, h, plan);
    if (rc == EFS_OK) {
        fill_txid(h, &t);
        rc = efs_meta_execute_mtime(plan, &t, &ops, &ctx);
    }
    free(plan);
    return rc;
}

int server_raft_host_logical_resize(efs_ino_t ino, uint64_t size, uint64_t now,
                                    int *leader_hint)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_resize_plan plan;
    struct efs_txid t;
    const struct efs_meta_resize_ops ops = {
        host_resize_prepare, host_resize_decide, host_resize_resolve};
    int hint = -1, rc;
    struct host_resize_ctx ctx = {h, &hint};
    uint8_t need[2];
    if (leader_hint)
        *leader_hint = -1;
    if (!h || !h->running || !ino)
        return EFS_ERR_INVAL;
    host_need_both(need);
    /* Capture can encounter a newly activated lane after the initial inode
     * read. Establish authority on both implemented groups before collecting. */
    for (unsigned i = 0; i < 2; ++i) {
        if (!host_hosts(h, need[i])) {
            if (leader_hint)
                *leader_hint = host_pick_peer(h, need, 2, -1);
            return EFS_ERR_NOT_PRIMARY;
        }
        rc = host_read_index(h, need[i], &hint);
        if (rc != EFS_OK)
            goto done;
    }
    rc = efs_meta_capture_resize(h->kv, ino, size, now, host_txn_coord, h, &plan);
    if (rc == EFS_OK) {
        fill_txid(h, &t);
        rc = efs_meta_execute_resize(&plan, &t, &ops, &ctx);
    }
done:
    if (leader_hint)
        *leader_hint = hint;
    return rc;
}

int server_raft_host_lane_bootstrap(efs_ino_t ino, uint64_t gen,
                                     uint32_t ci, uint32_t cs, int *leader_hint)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_lane_bootstrap q;
    struct efs_meta_writer_view ready;
    struct efs_txn_parts parts = {0};
    struct efs_txid t;
    int hint = -1, rc = EFS_ERR_INVAL;
    struct host_resize_ctx ctx = {h, &hint};
    uint8_t groups[2] = {efs_raft_shard_group(efs_kv_inode_shard(ino)),
                         efs_raft_shard_group(efs_kv_lane_shard(ino, ci % EFS_META_LANES))};
    int ng = groups[0] == groups[1] ? 1 : 2;
    if (leader_hint)
        *leader_hint = -1;
    if (!h || !h->running || !ino || !gen || !efs_chunk_size_valid(cs))
        return rc;
    if (!host_hosts(h, groups[0]) || !host_hosts(h, groups[ng - 1])) {
        hint = host_pick_peer(h, groups, ng, -1);
        rc = EFS_ERR_NOT_PRIMARY;
        goto done;
    }
    for (int i = 0; i < ng; ++i) {
        rc = host_read_index(h, groups[i], &hint);
        if (rc != EFS_OK)
            goto done;
    }
    rc = efs_meta_get_lane_writer_view(h->kv, ino, gen, ci, cs, &ready);
    if (rc != EFS_ERR_NOT_FOUND)
        goto done;
    rc = efs_meta_capture_lane_bootstrap(h->kv, ino, gen, ci, cs, host_txn_coord, h, &q);
    if (rc != EFS_OK)
        goto done;
    parts.shard[parts.n++] = efs_kv_inode_shard(ino);
    uint32_t lsh = efs_kv_lane_shard(ino, q.lane);
    if (lsh != parts.shard[0])
        parts.shard[parts.n++] = lsh;
    fill_txid(h, &t);
    for (int i = 0; i < 2; ++i) {
        uint8_t key[EFS_KV_KEY_MAX], pay[EFS_META_LANE_BOOTSTRAP_BYTES];
        uint32_t kl;
        rc = efs_meta_encode_lane_bootstrap(&q, i ? q.lane : EFS_META_FENCE_INODE, key, &kl, pay);
        if (rc == EFS_OK)
            rc = host_prep_raw(h, ((uint32_t)key[0] << 8) | key[1], EFS_TXN_LANE_BOOTSTRAP,
                                &t, &parts, key, kl, pay, sizeof(pay), NULL, &hint);
        if (rc != EFS_OK) {
            int abort_rc = host_resize_decide(&ctx, &t, &parts, EFS_TXN_ABORT);
            if (abort_rc != EFS_OK)
                rc = abort_rc;
            else
                (void)host_resize_resolve(&ctx, &t, &parts, EFS_TXN_ABORT);
            goto done;
        }
    }
    rc = host_resize_decide(&ctx, &t, &parts, EFS_TXN_COMMIT);
    if (rc == EFS_OK)
        rc = host_resize_resolve(&ctx, &t, &parts, EFS_TXN_COMMIT);
    /* Ambiguous COMMIT retains intents for recovery; never abort it. */
done:
    if (leader_hint)
        *leader_hint = hint;
    return rc;
}

/* Cross-group leftover migrate: hashed PUT + local DEL + used_shards
 * as a txn so group-2-only replicas see the hashed dentry. Same-group
 * leftovers (or hashed already present) stay a single DIR_MIGRATE apply.
 * Bounces to a dual-host when this replica does not host
 * the dest group. */
static int host_dir_migrate_txn(struct efs_raft_host *h, efs_ino_t dir,
                                const char *name, uint32_t hsh, int *hint)
{
    struct efs_meta_row row;
    struct efs_meta_dentry dent;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct efs_txn_ino_delta pd;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES];
    uint8_t loc_buf[EFS_META_DENT_BYTES];
    uint32_t kl = 0, kh = 0, ki = 0, ks = 0, locn;
    uint32_t psh, coord;
    uint64_t loc_ver = 0, hash_ver = 0, bit;
    uint8_t lane;
    int rc, i, stamp_ino = 0;

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
        /* first-use bit as a commutative OR on the dir row (§7.2) */
        memset(&pd, 0, sizeof(pd));
        pd.or_used_shards = bit;
        stamp_ino = 1;
        rc = efs_kv_key_inode(psh, dir, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
    }
    rc = efs_kv_key_dseq(hsh, dir, lane, k_dseq, &ks);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(h->kv, k_loc, kl, &loc_ver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_hash, kh, &hash_ver);
    if (rc != EFS_OK)
        return rc;
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
                rc = host_prep_ino_delta(h, psh, &t, &parts, k_ino, ki, &pd,
                                         NULL, hint);
        }
        if (rc == EFS_OK && sh == hsh) {
            rc = host_prep(h, hsh, EFS_TXN_EXCL, &t, &parts, k_hash, kh,
                           hash_ver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                           hint);
            if (rc == EFS_OK)
                rc = host_prep_dseq_bump(h, hsh, &t, &parts, k_dseq, ks, NULL,
                                         hint);
        }
    }
    if (rc != EFS_OK)
        (void)host_drop_parts(h, &t, &parts, hint);
    else {
        coord = efs_txn_coordinator(&t, &parts);
        rc = host_txn_commit(h, &t, &parts, coord, hint);
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
        rid = host_pick_peer(h, need, 2, -1);
        if (rid < 0)
            rc = EFS_ERR_NOT_PRIMARY;
        else
            rc = host_rpc_submit(h, rid, pg, cmd, clen, &rep, EFS_IO_TIMEOUT_MS);
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
        rc = host_read_index(h, pg, &hint);
        if (rc == EFS_OK) {
            struct efs_meta_row row;

            rc = efs_meta_apply_get_inode(h->kv, dir, &row);
            if (rc != EFS_OK || row.layout != EFS_META_LAYOUT_SPLITTING)
                continue;
            rc = host_dir_migrate(h, dir, cmd, HOST_DIR_LEN, &hint);
        } else {
            efs_dir_spread_note(dir);
            return;
        }
        if (rc == EFS_ERR_NOT_FOUND) {
            cmd[1] = EFS_MD_DIR_FINISH;
            rc = host_propose_wait(h, pg, cmd, HOST_DIR_LEN, &hint);
            if (rc != EFS_OK)
                efs_dir_spread_note(dir);
            continue;
        }
        efs_dir_spread_note(dir);
        if (rc != EFS_OK)
            return;
    }
}

/* LOCAL / HASHED / SPLITTING name drop matching apply dentry_drop_items
 * and sim drop_dentry_prep. SPLITTING writes HASHED=TOMBSTONE(epoch)
 * (I8) and DELs the local leftover unless the keys alias (lane 0). */
static int host_dent_drop_prep(struct efs_raft_host *h, uint32_t sh,
                               const struct efs_meta_dentry_drop *d,
                               const struct efs_txid *t,
                               const struct efs_txn_parts *parts, int *hint)
{
    int rc = EFS_OK;
    if ((d->put_tomb || d->del_hash) && sh == d->hsh)
        rc = host_prep_value(h, d->hsh, t, parts, d->k_hash, d->kh,
                              d->hash_value, d->hash_len,
                              d->put_tomb ? EFS_TXN_PUT : EFS_TXN_DEL,
                              d->put_tomb ? d->v_tomb : NULL,
                              d->put_tomb ? sizeof(d->v_tomb) : 0, hint);
    if (rc == EFS_OK && d->del_loc && sh == d->psh)
        rc = host_prep_value(h, d->psh, t, parts, d->k_loc, d->kl,
                              d->loc_value, d->loc_len, EFS_TXN_DEL, NULL, 0, hint);
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
                                  const struct efs_opid_req *q, int *hint)
{
    struct efs_meta_row crow;
    struct efs_meta_dentry dent;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct efs_txn_ino_delta pd;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES];
    uint8_t v_cino[EFS_META_INO_BYTES], v_alloc[EFS_META_ALLOC_BYTES];
    uint32_t kd = 0, kpi = 0, kci = 0, ka = 0, ks = 0;
    uint32_t psh, coord;
    uint64_t aver = 0, dver = 0;
    uint64_t bit;
    uint8_t lane;
    efs_ino_t next = 0, ino = 0;
    int rc, i;

    psh = efs_kv_inode_shard(parent);
    lane = efs_kv_dir_lane(name);
    bit = 1ull << lane;
    prow->used_shards |= bit;
    /* Parent home row: first-use used_shards bit only, as a commutative
     * OR (times ride the dir-lane). */
    memset(&pd, 0, sizeof(pd));
    pd.or_used_shards = bit;
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
        rc = efs_txn_ver_get(h->kv, k_alloc, ka, &aver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK) {
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
                rc = host_prep_ino_delta(h, psh, &t, &parts, k_pino, kpi, &pd,
                                         NULL, hint);
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
                    rc = host_prep_dseq_bump(h, dsh, &t, &parts, k_dseq, ks,
                                             NULL, hint);
                if (rc == EFS_OK)
                    rc = host_prep_lane_stamp(h, dsh, &t, &parts, prow, name,
                                              at->now, NULL, hint);
                if (rc == EFS_OK)
                    rc = host_prep_opid(h, dsh, &t, &parts, q, ino, 0, NULL,
                                        hint);
            }
        }
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            rc = host_txn_commit(h, &t, &parts, coord, hint);
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
                            const struct efs_opid_req *q,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, crow;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct efs_txn_ino_delta pd;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES];
    uint8_t v_cino[EFS_META_INO_BYTES], v_alloc[EFS_META_ALLOC_BYTES];
    uint32_t kd = 0, kpi = 0, kci = 0, ka = 0, ks = 0;
    uint32_t psh, csh, dsh, coord;
    uint64_t aver = 0, dver = 0, salt = 0;
    uint64_t now;
    efs_ino_t next = 0, ino = 0;
    uint8_t mkcmd[HOST_CMD_MAX], p_lane = 0;
    uint32_t mklen = 0;
    struct host_idx_ref prefs[8];
    int hint = -1;
    int rc, i, np = 0;
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
        host_fwd_create(h, parent, name, mode, uid, gid, 0, 0, q, out, need, 2);
        return;
    }
    stage = 1;
    rc = host_export_salt(h, &salt, &hint);
    if (rc == EFS_OK)
        stage = 2;
    csh = (rc == EFS_OK) ? efs_kv_mkdir_shard(parent, name, salt) : 0;
    if (rc == EFS_OK && !host_hosts(h, efs_raft_shard_group(csh))) {
        uint8_t need[2];
        int nn = 1;
        need[0] = efs_raft_shard_group(psh);
        if (efs_raft_shard_group(csh) != need[0])
            need[nn++] = efs_raft_shard_group(csh);
        host_fwd_create(h, parent, name, mode, uid, gid, 0, 0, q, out, need, nn);
        return;
    }
    if (rc == EFS_OK) {
        stage = 3;
        rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    }
    if (rc == EFS_OK && efs_raft_shard_group(csh) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(csh), &hint);
    if (rc == EFS_OK) {
        stage = 4;
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    }
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
            host_fwd_create(h, parent, name, mode, uid, gid, 0, 0, q, out, need,
                            nn);
            return;
        }
        if (efs_raft_shard_group(dsh) != efs_raft_shard_group(psh) &&
            efs_raft_shard_group(dsh) != efs_raft_shard_group(csh))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    } else
        dsh = 0;
    if (rc == EFS_OK) {
        struct efs_opid_reply rep;
        int hit = host_opid_replay(h, dsh, q, &rep);

        if (hit < 0) {
            rc = hit;
        } else if (hit) {
            host_opid_reply_ino(h, parent, name, &rep, out);
            return;
        }
    }
    if (rc == EFS_OK) {
        stage = 5;
        rc = efs_meta_apply_lookup_tx(h->kv, parent, name, host_txn_coord, h, &dent);
    }
    if (rc == EFS_OK) {
        set_inode_rc(out, EFS_ERR_EXIST, hint);
        return;
    }
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    if (rc == EFS_OK &&
        efs_raft_shard_group(psh) == efs_raft_shard_group(csh) &&
        efs_raft_shard_group(psh) == efs_raft_shard_group(dsh)) {
        struct efs_meta_attrs at2;

        stage = 6;
        at2.uid = uid;
        at2.gid = gid;
        at2.now = now;
        rc = efs_meta_apply_peek_alloc(h->kv, csh, &next);
        if (rc == EFS_OK) {
            if (efs_kv_inode_shard(next) != csh)
                rc = EFS_ERR_PROTO;
            else
                rc = pack_create_cmd(mkcmd, &mklen, parent, mode, name, &at2, 0,
                                     next, prow.layout, q);
        }
        if (rc == EFS_OK)
            rc = host_propose_wait(h, efs_raft_shard_group(psh), mkcmd, mklen,
                                   &hint);
        goto mkdir_done;
    }
    if (rc == EFS_OK) {
        stage = 6;
        rc = efs_meta_apply_peek_alloc(h->kv, csh, &next);
    }
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
        /* Parent row as a commutative delta (§7.2): nlink++ always; for a
         * LOCAL parent the child count and the times too; for HASHED /
         * SPLITTING the times live on the dir-lane (§7.4) and the home row
         * takes only the first-use used_shards bit. */
        memset(&pd, 0, sizeof(pd));
        pd.d_nlink = 1;
        if (prow.layout == EFS_META_LAYOUT_LOCAL) {
            pd.d_nents = 1;
            pd.max_mtime = now;
            pd.max_ctime = now;
        } else {
            pd.or_used_shards = 1ull << efs_kv_dir_lane(name);
            stamp_lane = 1;
        }
        rc = efs_meta_pack_dentry(&dent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&crow, v_cino, sizeof(v_cino));
        wr64be(v_alloc, next);
    }
    if (rc == EFS_OK) {
        stage = 7;
        rc = efs_kv_key_dentry(dsh, parent, name, k_dent, &kd);
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(csh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, p_lane, k_dseq, &ks);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_alloc, ka, &aver);
    if (rc == EFS_OK) {
        stage = 8;
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    }
    if (rc == EFS_OK) {
        memset(&parts, 0, sizeof(parts));
        rc = host_parts_add(&parts, psh);
        if (rc == EFS_OK)
            rc = host_parts_add(&parts, csh);
        if (rc == EFS_OK)
            rc = host_parts_add(&parts, dsh);
        if (rc != EFS_OK)
            goto mkdir_prepped;
        stage = 9;
        fill_txid(h, &t);
        np = 0;
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == dsh) {
                rc = host_prep_async(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dent,
                                     kd, dver, EFS_TXN_PUT, v_dent,
                                     sizeof(v_dent), &prefs[np], &hint);
                if (rc == EFS_OK)
                    np++;
                if (rc == EFS_OK)
                    rc = host_prep_dseq_bump(h, dsh, &t, &parts, k_dseq, ks,
                                             &prefs[np], &hint);
                if (rc == EFS_OK)
                    np++;
                if (rc == EFS_OK && stamp_lane)
                    rc = host_prep_lane_stamp(h, dsh, &t, &parts, &prow, name,
                                              now, &prefs[np], &hint);
                if (rc == EFS_OK && stamp_lane)
                    np++;
                if (rc == EFS_OK && host_has_opid(q))
                    rc = host_prep_opid(h, dsh, &t, &parts, q, ino, 0,
                                        &prefs[np], &hint);
                if (rc == EFS_OK && host_has_opid(q))
                    np++;
            }
            if (rc == EFS_OK && sh == psh) {
                rc = host_prep_ino_delta(h, psh, &t, &parts, k_pino, kpi, &pd,
                                         &prefs[np], &hint);
                if (rc == EFS_OK)
                    np++;
            }
            if (rc == EFS_OK && sh == csh) {
                rc = host_prep_async(h, csh, EFS_TXN_EXCL, &t, &parts, k_cino,
                                     kci, 0, EFS_TXN_PUT, v_cino,
                                     sizeof(v_cino), &prefs[np], &hint);
                if (rc == EFS_OK)
                    np++;
                if (rc == EFS_OK)
                    rc = host_prep_async(h, csh, EFS_TXN_EXCL, &t, &parts,
                                         k_alloc, ka, aver, EFS_TXN_PUT,
                                         v_alloc, sizeof(v_alloc), &prefs[np],
                                         &hint);
                if (rc == EFS_OK)
                    np++;
            }
        }
        if (rc == EFS_OK)
            rc = host_wait_refs(h, prefs, np, &hint);
mkdir_prepped:
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            stage = 10;
            coord = efs_txn_coordinator(&t, &parts);
            rc = host_txn_commit(h, &t, &parts, coord, &hint);
        }
    }
mkdir_done:
    stage = 11;
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup_tx(h->kv, parent, name, host_txn_coord, h, &dent);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, dent.ino, &crow);
    if (rc == EFS_OK)
        host_stat_from_row(&crow, &st);
    if (dirop_fail_on(rc))
        fprintf(stderr, "raft-host: mkdir parent=%llu name=%s rc=%d "
                "hint=%d psh=%u csh=%u dsh=%u stage=%d ino=%llu\n",
                (unsigned long long)parent, name, rc, hint, psh,
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
                            const struct efs_opid_req *q,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    struct efs_meta_dir_cursor cur;
    struct efs_meta_dir_ent one;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct host_pver_guard gv[EFS_TXN_NAMESPACE_MAX_PART];
    struct efs_meta_dentry_drop drop;
    struct efs_txn_ino_delta pd;
    uint8_t k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_pdseq[EFS_KV_KEY_MAX], k_cdseq[EFS_KV_KEY_MAX];
    uint32_t kpi = 0, kci = 0, kps = 0, kcs = 0, nent = 0;
    uint32_t psh, csh, dsh, coord;
    uint64_t cver = 0, gver = 0, now;
    uint8_t rmcmd[HOST_CMD_MAX], p_lane = 0;
    struct host_idx_ref prefs[16];
    int hint = -1;
    int rc, i, held, stamp_lane = 0, ngv = 0, hashed_child = 0, npref = 0;
    uint32_t rmlen = 0;

    memset(out, 0, sizeof(*out));
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
        host_fwd_unlink(h, parent, name, 1, q, out, need, 2);
        return;
    }
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
            host_fwd_unlink(h, parent, name, 1, q, out, need, nn);
            return;
        }
        if (efs_raft_shard_group(dsh) != efs_raft_shard_group(psh))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    } else
        dsh = 0;
    if (rc == EFS_OK) {
        /* I16: a committed rmdir's retry finds no dentry; answer the
         * recorded OK instead of ENOENT. */
        struct efs_opid_reply rep;
        int hit = host_opid_replay(h, dsh, q, &rep);

        if (hit < 0) {
            rc = hit;
        } else if (hit) {
            set_inode_rc(out, rep.rc, hint);
            if (rep.rc == EFS_OK) {
                out->inode.ino = rep.ino;
                out->inode.mode = S_IFDIR | 0755;
                out->inode.nlink = 2;
                out->inode.parent = parent;
                strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
            }
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup_tx(h->kv, parent, name, host_txn_coord, h, &dent);
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
            host_fwd_unlink(h, parent, name, 1, q, out, need, nn);
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_resolve_tx(h->kv, parent, name, host_txn_coord, h, &dent, &row);
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
            host_fwd_unlink(h, parent, name, 1, q, out, gs, ngs);
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
            if (ngv >= EFS_TXN_NAMESPACE_MAX_PART) {
                rc = EFS_ERR_BUSY;
                break;
            }
            lsh = efs_kv_lane_shard(row.ino, (uint8_t)lane);
            gv[ngv].shard = lsh;
            gv[ngv].klen = 0;
            rc = efs_kv_key_dseq(lsh, row.ino, (uint8_t)lane, gv[ngv].key,
                                  &gv[ngv].klen);
            /* dseq GUARDs are by value (efs_txn_prepare_guard). */
            if (rc == EFS_OK)
                rc = efs_txn_dseq_observe(h->kv, gv[ngv].key, gv[ngv].klen,
                                          &gv[ngv].ver);
            if (rc == EFS_OK)
                ngv++;
        }
    }
    if (rc == EFS_OK) {
        /* Parent row delta (§7.2): nlink--, and for a LOCAL parent the
         * child count and times; HASHED/SPLITTING times go to the lane. */
        memset(&pd, 0, sizeof(pd));
        pd.d_nlink = -1;
        if (prow.layout == EFS_META_LAYOUT_LOCAL) {
            pd.d_nents = -1;
            pd.max_mtime = now;
            pd.max_ctime = now;
        } else
            stamp_lane = 1;
    }
    if (rc == EFS_OK)
        rc = efs_meta_capture_dentry_drop(h->kv, &prow, parent, name, &dent, &drop);
    if (rc == EFS_OK) {
        uint8_t g0 = efs_raft_shard_group(psh);
        int same = 1;

        if (efs_raft_shard_group(dsh) != g0)
            same = 0;
        if (efs_raft_shard_group(csh) != g0)
            same = 0;
        if (drop.del_loc && efs_raft_shard_group(drop.psh) != g0)
            same = 0;
        if ((drop.put_tomb || drop.del_hash) &&
            efs_raft_shard_group(drop.hsh) != g0)
            same = 0;
        for (i = 0; i < ngv && same; i++) {
            if (efs_raft_shard_group(gv[i].shard) != g0)
                same = 0;
        }
        if (same) {
            rc = pack_rmdir_cmd(rmcmd, &rmlen, parent, now, name, q);
            if (rc == EFS_OK)
                rc = host_propose_wait(h, g0, rmcmd, rmlen, &hint);
            goto rmdir_done;
        }
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, row.ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, p_lane, k_pdseq, &kps);
    if (rc == EFS_OK && !hashed_child)
        rc = efs_kv_key_dseq(csh, row.ino, 0, k_cdseq, &kcs);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_cino, kci, &cver);
    if (rc == EFS_OK && !hashed_child)
        rc = efs_txn_dseq_observe(h->kv, k_cdseq, kcs, &gver);
    if (rc == EFS_OK) {
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
        npref = 0;
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            int g;
            rc = host_dent_drop_prep(h, sh, &drop, &t, &parts, &hint);
            if (rc == EFS_OK && sh == dsh) {
                if (npref >= 16)
                    rc = EFS_ERR_BUSY;
                else
                    rc = host_prep_dseq_bump(h, dsh, &t, &parts, k_pdseq, kps,
                                             &prefs[npref], &hint);
                if (rc == EFS_OK)
                    npref++;
                if (rc == EFS_OK && stamp_lane) {
                    if (npref >= 16)
                        rc = EFS_ERR_BUSY;
                    else
                        rc = host_prep_lane_stamp(h, dsh, &t, &parts, &prow,
                                                  name, now, &prefs[npref],
                                                  &hint);
                    if (rc == EFS_OK)
                        npref++;
                }
                if (rc == EFS_OK && host_has_opid(q)) {
                    if (npref >= 16)
                        rc = EFS_ERR_BUSY;
                    else
                        rc = host_prep_opid(h, dsh, &t, &parts, q, row.ino, 0,
                                            &prefs[npref], &hint);
                    if (rc == EFS_OK)
                        npref++;
                }
            }
            if (rc == EFS_OK && sh == psh) {
                if (npref >= 16)
                    rc = EFS_ERR_BUSY;
                else
                    rc = host_prep_ino_delta(h, psh, &t, &parts, k_pino, kpi,
                                             &pd, &prefs[npref], &hint);
                if (rc == EFS_OK)
                    npref++;
            }
            if (rc == EFS_OK && sh == csh) {
                if (npref >= 16)
                    rc = EFS_ERR_BUSY;
                else
                    rc = host_prep_async(h, csh, EFS_TXN_EXCL, &t, &parts,
                                         k_cino, kci, cver, EFS_TXN_DEL, NULL,
                                         0, &prefs[npref], &hint);
                if (rc == EFS_OK)
                    npref++;
                if (rc == EFS_OK && !hashed_child) {
                    if (npref >= 16)
                        rc = EFS_ERR_BUSY;
                    else
                        rc = host_prep_async(h, csh, EFS_TXN_GUARD, &t, &parts,
                                             k_cdseq, kcs, gver, 0, NULL, 0,
                                             &prefs[npref], &hint);
                    if (rc == EFS_OK)
                        npref++;
                }
            }
            for (g = 0; g < ngv && rc == EFS_OK; g++) {
                if (gv[g].shard != sh)
                    continue;
                if (npref >= 16)
                    rc = EFS_ERR_BUSY;
                else
                    rc = host_prep_async(h, gv[g].shard, EFS_TXN_GUARD, &t,
                                         &parts, gv[g].key, gv[g].klen,
                                         gv[g].ver, 0, NULL, 0, &prefs[npref],
                                         &hint);
                if (rc == EFS_OK)
                    npref++;
            }
        }
        if (rc == EFS_OK)
            rc = host_wait_refs(h, prefs, npref, &hint);
rmdir_prepped:
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            rc = host_txn_commit(h, &t, &parts, coord, &hint);
        }
    }
rmdir_done:
    if (dirop_fail_on(rc))
        fprintf(stderr, "raft-host: rmdir parent=%llu name=%s rc=%d hint=%d\n",
                (unsigned long long)parent, name, rc, hint);
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
                            const struct efs_opid_req *q,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct efs_meta_dentry_drop drop;
    struct efs_txn_ino_delta pd, id;
    uint8_t k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint32_t ki = 0, kp = 0, ks = 0, krl = 0;
    uint32_t dsh, ish, psh, coord, ash = 0;
    uint64_t iver = 0, now;
    uint64_t rver = 0;
    uint32_t nlink_out = 0;
    int hint = -1;
    int rc, i, held = 0, last = 0, put_ino = 0;
    int touch_parent = 0, stamp_lane = 0;
    uint8_t d_lane = 0;

    memset(out, 0, sizeof(*out));
    memset(&drop, 0, sizeof(drop));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0 || name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    now = now_ns();
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
        rc = efs_meta_apply_resolve_tx(h->kv, parent, name, host_txn_coord, h, &dent, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    ish = (rc == EFS_OK) ? efs_kv_inode_shard(row.ino) : 0;
    if (rc == EFS_OK && efs_raft_shard_group(ish) != efs_raft_shard_group(dsh))
        rc = host_read_index(h, efs_raft_shard_group(ish), &hint);
    if (rc == EFS_OK && !host_row_lanes_hosted(h, row.ino, &row)) {
        uint8_t need[2];

        host_need_both(need);
        host_fwd_unlink(h, parent, name, 0, q, out, need, 2);
        return;
    }
    /* The file row's nlink-- (or the held last-link's nlink=0) is a delta
     * too: PUBLISH and setattr write the same row on the log path, and a
     * full image from this read snapshot would overwrite them. */
    memset(&id, 0, sizeof(id));
    id.d_nlink = -1;
    id.max_ctime = now;
    if (rc == EFS_OK) {
        nlink_out = row.nlink;
        last = row.nlink <= 1;
        if (last) {
            held = efs_lease_any(h->kv, row.ino, row.generation);
            if (held < 0)
                rc = held;
            else if (held)
                put_ino = 1;
        } else
            put_ino = 1;
    }
    if (rc == EFS_OK) {
        d_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
        memset(&pd, 0, sizeof(pd));
        if (prow.layout == EFS_META_LAYOUT_LOCAL) {
            pd.d_nents = -1;
            pd.max_mtime = now;
            pd.max_ctime = now;
            touch_parent = 1;
        } else
            stamp_lane = 1;
    }
    if (rc == EFS_OK)
        rc = efs_meta_capture_dentry_drop(h->kv, &prow, parent, name, &dent, &drop);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, row.ino, k_ino, &ki);
    if (rc == EFS_OK && touch_parent)
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, d_lane, k_dseq, &ks);
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
    if (rc == EFS_OK && !put_ino)
        rc = efs_txn_ver_get(h->kv, k_ino, ki, &iver);
    if (rc == EFS_OK) {
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
                rc = host_prep_dseq_bump(h, dsh, &t, &parts, k_dseq, ks, NULL,
                                         &hint);
                if (rc == EFS_OK && stamp_lane)
                    rc = host_prep_lane_stamp(h, dsh, &t, &parts, &prow, name,
                                              now, NULL, &hint);
                if (rc == EFS_OK)
                    rc = host_prep_opid(h, dsh, &t, &parts, q, row.ino,
                                        last ? 0 : row.nlink - 1, NULL, &hint);
            }
            if (rc == EFS_OK && touch_parent && sh == psh)
                rc = host_prep_ino_delta(h, psh, &t, &parts, k_par, kp, &pd,
                                         NULL, &hint);
            if (rc == EFS_OK && sh == ish) {
                if (put_ino)
                    rc = host_prep_ino_delta(h, ish, &t, &parts, k_ino, ki,
                                             &id, NULL, &hint);
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
            rc = host_txn_commit(h, &t, &parts, coord, &hint);
        }
    }
    if (rc == EFS_OK && !last) {
        rc = host_read_inode_lanes(h, row.ino, &hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_getattr(h->kv, row.ino, host_txn_coord, h, &st);
    }
    if (dirop_fail_on(rc))
        fprintf(stderr,
                "raft-host: unlink-txn parent=%llu name=%s rc=%d hint=%d "
                "last=%d held=%d dsh=%u ish=%u psh=%u\n",
                (unsigned long long)parent, name, rc, hint, last, held,
                dsh, ish, psh);
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
                             const struct efs_opid_req *q,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow = {0}, row;
    struct efs_meta_dentry dent = {0};
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t clen = 0;
    uint64_t idx = 0, term = 0;
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
        server_raft_host_rmdir(parent, name, q, out);
        return;
    }
    /* The unlink may need the inode's group as well as the parent/dentry
     * group (last-link frees the inode row). Forward to a host that has
     * every group this op can touch rather than failing ReadIndex with an
     * unactionable NOT_PRIMARY. */
    if (!host_hosts(h, efs_raft_shard_group(efs_kv_inode_shard(parent)))) {
        uint8_t need[2];
        host_need_both(need);
        host_fwd_unlink(h, parent, name, 0, q, out, need, 2);
        return;
    }
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
            host_fwd_unlink(h, parent, name, 0, q, out, need, nn);
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
        /* I16: the retry of a committed unlink finds no dentry — answer
         * the recorded OK (ino, nlink after) instead of ENOENT. */
        struct efs_opid_reply rep;
        int hit = host_opid_replay(h, dsh, q, &rep);

        if (hit < 0) {
            rc = hit;
        } else if (hit) {
            set_inode_rc(out, rep.rc, hint);
            if (rep.rc == EFS_OK) {
                struct efs_meta_row r2;
                struct efs_meta_stat st2;

                if (rep.extra > 0 &&
                    efs_meta_apply_get_inode(h->kv, rep.ino, &r2) == EFS_OK) {
                    host_stat_from_row(&r2, &st2);
                    stat_to_inode(&st2, &out->inode);
                } else {
                    out->inode.ino = rep.ino;
                    out->inode.mode = S_IFREG;
                    out->inode.nlink = (uint32_t)(rep.extra ? rep.extra : 1);
                }
                out->inode.parent = parent;
                strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
            }
            return;
        }
    }
    if (rc == EFS_OK) {
        /* Dentry-first: the inode row can live on a group this host does
         * not host, and resolve would then fail I9 (EFS_ERR_IO) before the
         * hosting check below ever runs. Look the dentry up locally, bounce
         * to a host that has the inode's group, and only then resolve. */
        rc = efs_meta_apply_lookup_tx(h->kv, parent, name, host_txn_coord, h, &dent);
        if (rc == EFS_OK && (dent.type & S_IFMT) == S_IFDIR) {
            server_raft_host_rmdir(parent, name, q, out);
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
                host_fwd_unlink(h, parent, name, 0, q, out, need, nn);
                return;
            }
        }
        if (rc == EFS_OK)
            rc = efs_meta_apply_resolve_tx(h->kv, parent, name, host_txn_coord, h, &dent, &row);
        if (rc == EFS_OK && !host_row_lanes_hosted(h, row.ino, &row)) {
            uint8_t need[2];

            host_need_both(need);
            host_fwd_unlink(h, parent, name, 0, q, out, need, 2);
            return;
        }
        if (rc == EFS_OK) {
            if (row.nlink > 1 || ig != dg ||
                (prow.layout == EFS_META_LAYOUT_SPLITTING && pg != dg)) {
                host_unlink_txn(parent, name, q, out);
                return;
            }
        }
    }
    /* Legacy W36 names can outlive their inode. Let the single-shard apply
     * validate absence/intents and atomically remove only that dangling name.
     * Cross-shard and directory repairs require a separate recovery design. */
    if ((rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_IO) &&
        prow.layout == EFS_META_LAYOUT_LOCAL &&
        dent.ino && (dent.type & S_IFMT) == S_IFREG &&
        efs_kv_inode_shard(dent.ino) == efs_kv_inode_shard(parent)) {
        memset(&row, 0, sizeof(row));
        row.ino = dent.ino; row.mode = dent.type;
        rc = EFS_OK;
    }
    if (rc == EFS_OK)
        rc = pack_unlink_cmd(cmd, &clen, parent, now_ns(), name, q);
    if (rc == EFS_OK)
        rc = host_propose(h, dg, cmd, clen, &idx, &term, &hint);
    if (rc == EFS_OK)
        rc = host_wait_verdict(h, dg, idx, term, &hint);
    if (dirop_fail_on(rc))
        fprintf(stderr,
                "raft-host: unlink-simple parent=%llu name=%s rc=%d hint=%d "
                "dsh=%u dg=%u\n",
                (unsigned long long)parent, name, rc, hint, dsh, dg);
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
                         uint32_t atime_nsec,
                         struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_utimens u;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    uint8_t cmd[HOST_UTIMENS_LEN];
    uint32_t clen = 0;
    uint64_t idx = 0, term = 0, bits;
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
        u.atime = atime * 1000000000ull + (uint64_t)atime_nsec;
    }
    u.expect_gen = 0;
    g = efs_raft_shard_group(efs_kv_inode_shard(ino));
    rc = host_read_index(h, g, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && S_ISREG(row.mode) && (u.mask & EFS_META_SET_MTIME)) {
        uint8_t need[2]; host_need_both(need);
        if (!host_hosts(h, need[0]) || !host_hosts(h, need[1])) {
            host_fwd_setattr(h, ino, mask, 0, 0, 0, 0, mtime, mtime_nsec,
                             atime, atime_nsec, out, need, 2);
            return;
        }
        rc = host_regular_mtime(h, ino, &u, &hint);
        goto collect;
    }
    if (rc == EFS_OK && (u.mask & EFS_META_SET_MTIME)) {
        uint64_t cross = 0;
        uint8_t fg = g;

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
            if (lg != g) {
                cross |= 1ULL << i;
                fg = lg;
            }
        }
        /* Lane records in the other group are updated by the same
         * utimens command applied there (inode row is not in that KV;
         * lane_bits says which lanes). Proposed first, like truncate's
         * LANE_FENCE, so a REPORT cannot stamp "now" over the set time. */
        if (rc == EFS_OK && cross) {
            if (!host_hosts(h, fg)) {
                uint8_t need[2];
                host_need_both(need);
                host_fwd_setattr(h, ino, mask, 0, 0, 0, 0, mtime, mtime_nsec,
                                 atime, atime_nsec, out, need, 2);
                return;
            }
            u.expect_gen = row.generation;
            u.lane_bits = cross;
            rc = pack_utimens_cmd(cmd, &clen, ino, now_ns(), &u);
            if (rc == EFS_OK)
                rc = host_propose_wait(h, fg, cmd, clen, &hint);
            u.expect_gen = 0;
            u.lane_bits = 0;
        }
    }
    if (rc == EFS_OK)
        rc = pack_utimens_cmd(cmd, &clen, ino, now_ns(), &u);
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &term, &hint);
    if (rc == EFS_OK)
        rc = host_wait_settled(h, g, idx, term, &hint);
collect:
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
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
    uint64_t idx = 0, term = 0, bits = 0, now, same_mask = 0;
    uint8_t g, lane = 0, lg = 0, has_tail = 0;
    int hint = -1, ncross = 0, tail_ext = 0, keep_tail = 0;
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
                         0, size, mtime, mtime_nsec, 0, 0, out, need, 2);
        return;
    }
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
            host_need_both(need);
            host_fwd_setattr(h, ino, EFS_SETATTR_SIZE | EFS_SETATTR_MTIME, 0,
                             0, 0, size, mtime, mtime_nsec, 0, 0, out, need, 2);
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
            tail.coding_profile_id = EFS_META_PROFILE_K2F1;
            if (got.generation != 0) {
                /* Alias the live objects under the generation they were
                 * PUT at. A fresh candidate_gen is a filename nothing
                 * wrote; the reader GETs that name and DECODE-fails
                 * (peer_truncate_visible, peer_extend_and_truncate).
                 * The epoch fence rejects a stale publish. The apply
                 * sees candidate == committed and leaves the row. */
                tail.candidate_gen = got.generation;
                memcpy(tail.ch.nodes, got.nodes, sizeof(tail.ch.nodes));
                memcpy(tail.ch.checksums, got.checksums,
                       sizeof(tail.ch.checksums));
                tp = &tail;
            } else {
                struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
                uint32_t nd = 0;

                /* A generation-0 row with a trailer is a span chain, not
                 * a hole. A zero stub drops the trailer
                 * (peer_truncate_visible read back zeros). Keep the row;
                 * the size change still hides bytes past EOF. */
                if (efs_meta_apply_get_chunk_deltas(h->kv, ino, tci, ds,
                                                    EFS_CHUNK_DELTA_MAX, &nd,
                                                    NULL) == EFS_OK &&
                    nd > 0) {
                    has_tail = 0;
                    tp = NULL;
                    tail_ext = 0;
                    keep_tail = 1;
                } else {
                    tail.candidate_gen = efs_meta_candidate_gen(uuid, 0, 2, tci, 0);
                    if (tail.candidate_gen == 0 ||
                        tail.candidate_gen == tail.expected_gen)
                        tail.candidate_gen = tail.expected_gen + 1;
                    if (tail.candidate_gen == 0)
                        tail.candidate_gen = 1;
                    /* Grow into a chunk that was never written: publish the
                     * well-known zero-fragment digests so the read path
                     * synthesizes zeros without a GET (no fragments exist). */
                    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                        tail.ch.nodes[i] = (efs_node_id_t)(i + 1);
                        efs_hash_zero_fragment(tail.ch.checksums[i]);
                    }
                    tp = &tail;
                }
            }
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
                               (uint8_t)((tail_ext ? HOST_TRUNC_F_TAIL_EXT : 0) |
                                         (keep_tail ? HOST_TRUNC_F_KEEP_TAIL : 0)));
    if (rc == EFS_OK && clen > HOST_CMD_MAX)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &term, &hint);
    if (rc == EFS_OK)
        rc = host_wait_settled(h, g, idx, term, &hint);
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
            if (!got.generation) {
                struct efs_meta_delta deltas[EFS_CHUNK_DELTA_MAX];
                uint32_t count = 0;
                rc = efs_meta_apply_get_chunk_deltas(h->kv, ino, tci, deltas,
                                                    EFS_CHUNK_DELTA_MAX, &count,
                                                    NULL);
                if (rc == EFS_OK && count) {
                    /* A span-only tail is live data, just as in the
                     * same-group branch. The committed fence hides EOF;
                     * do not replace its trailer with a zero stub. */
                    break;
                }
                if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
                    break;
                rc = EFS_OK;
            }
            memset(&tail, 0, sizeof(tail));
            memset(uuid, 0, sizeof(uuid));
            tail.ino = ino;
            tail.chunk_index = tci;
            tail.new_size = size;
            tail.expected_gen = got.generation;
            tail.coding_profile_id = EFS_META_PROFILE_K2F1;
            tail.content_epoch = row.content_epoch + 1;
            tail.inode_gen = row.generation;
            tail.mtime_gen = row.mtime_gen;
            tail.lane_local = 1;
            if (got.generation != 0) {
                /* Same alias rule as the same-group branch: the live
                 * generation already names the objects. */
                tail.candidate_gen = got.generation;
                memcpy(tail.ch.nodes, got.nodes, sizeof(tail.ch.nodes));
                memcpy(tail.ch.checksums, got.checksums,
                       sizeof(tail.ch.checksums));
            } else {
                tail.candidate_gen = efs_meta_candidate_gen(uuid, 0, 2, tci,
                                                            (uint32_t)attempt);
                if (tail.candidate_gen == 0 ||
                    tail.candidate_gen == tail.expected_gen)
                    tail.candidate_gen = tail.expected_gen + 1;
                if (tail.candidate_gen == 0)
                    tail.candidate_gen = 1;
                for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                    tail.ch.nodes[i] = (efs_node_id_t)(i + 1);
                    efs_hash_zero_fragment(tail.ch.checksums[i]);
                }
            }
            rc = pack_publish_cmd(cmd, &clen, &tail);
            if (rc == EFS_OK && clen > HOST_CMD_MAX)
                rc = EFS_ERR_INVAL;
            if (rc == EFS_OK)
                rc = host_propose(h, tg, cmd, clen, &idx, &term, &hint);
            if (rc == EFS_OK)
                rc = host_wait_settled(h, tg, idx, term, &hint);
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
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

void server_raft_host_setattr(efs_ino_t ino, uint32_t mask, uint32_t mode,
                              uint32_t uid, uint32_t gid, uint64_t size,
                              uint64_t mtime, uint32_t mtime_nsec,
                              uint64_t atime, uint32_t atime_nsec,
                              struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_setattr sa;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    uint8_t cmd[HOST_SETATTR_LEN];
    uint32_t clen = 0;
    uint64_t idx = 0, term = 0;
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
        host_utimens(ino, times, mtime, mtime_nsec, atime, atime_nsec, out);
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
    rc = host_read_index(h, g, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    /* A lane on the other group: do the setattr on a dual host, before
     * this one commits it and then fails the post-commit lane read. */
    if (rc == EFS_OK && !host_row_lanes_hosted(h, ino, &row)) {
        uint8_t need[2];

        host_need_both(need);
        host_fwd_setattr(h, ino, mask, mode, uid, gid, size, mtime,
                         mtime_nsec, atime, atime_nsec, out, need, 2);
        return;
    }
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &term, &hint);
    if (rc == EFS_OK)
        rc = host_wait_settled(h, g, idx, term, &hint);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

_Static_assert(offsetof(struct efs_msg_xattr_reply, primary_id) ==
                   offsetof(struct efs_msg_inode_reply, primary_id),
               "xattr reply status layout");
_Static_assert(offsetof(struct efs_msg_xattr_reply, data) == 12,
               "xattr reply header");
_Static_assert(sizeof(struct efs_msg_xattr) == 24, "xattr request layout");

void server_raft_host_xattr(efs_ino_t ino, uint8_t op, uint32_t flags,
                            const uint8_t *name, uint16_t nlen,
                            const uint8_t *val, uint32_t vlen,
                            struct efs_msg_xattr_reply *out, uint32_t *out_len)
{
    struct efs_raft_host *h = g_host;
    uint8_t cmd[HOST_XATTR_HDR + EFS_XATTR_NAME_MAX + EFS_XATTR_VALUE_MAX];
    uint32_t clen, hdr, n = 0;
    uint8_t g;
    int hint = -1;
    int rc;

    hdr = (uint32_t)offsetof(struct efs_msg_xattr_reply, data);
    memset(out, 0, hdr);
    out->status = EFS_INODE_RPC_ERROR;
    if (out_len)
        *out_len = hdr;
    if (!h || !h->running || ino == 0 || !out_len) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (nlen > EFS_XATTR_NAME_MAX || vlen > EFS_XATTR_VALUE_MAX ||
        (nlen && !name) || (vlen && !val) ||
        (op != EFS_XATTR_GET && op != EFS_XATTR_LIST &&
         op != EFS_XATTR_SET && op != EFS_XATTR_REMOVE)) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    g = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (op == EFS_XATTR_GET || op == EFS_XATTR_LIST) {
        n = EFS_XATTR_BLOB_MAX;
        rc = host_read_index(h, g, &hint);
        if (rc == EFS_OK) {
            if (op == EFS_XATTR_GET)
                rc = efs_meta_xattr_get(h->kv, ino, name, nlen, out->data, &n);
            else
                rc = efs_meta_xattr_list(h->kv, ino, out->data, &n);
        }
        out->status = rc_to_inode_status(rc);
        out->primary_id = (hint >= 0) ? (efs_node_id_t)(hint + 1) : 0;
        if (rc == EFS_OK) {
            out->nbytes = n;
            *out_len = hdr + n;
        }
        return;
    }
    cmd[0] = EFS_MD_CMD_XATTR;
    cmd[1] = op;
    wr64be(cmd + 2, ino);
    wr64be(cmd + 10, now_ns());
    wr32be(cmd + 18, flags);
    cmd[22] = (uint8_t)(nlen >> 8);
    cmd[23] = (uint8_t)nlen;
    wr32be(cmd + 24, vlen);
    if (nlen)
        memcpy(cmd + HOST_XATTR_HDR, name, nlen);
    if (vlen)
        memcpy(cmd + HOST_XATTR_HDR + nlen, val, vlen);
    clen = HOST_XATTR_HDR + (uint32_t)nlen + vlen;
    rc = host_propose_wait(h, g, cmd, clen, &hint);
    out->status = rc_to_inode_status(rc);
    out->primary_id = (hint >= 0) ? (efs_node_id_t)(hint + 1) : 0;
}

/* O_APPEND reserve. Reply size is THIS reservation's end (eof+len), not
 * the live watermark — two concurrent clients otherwise both write at
 * wm-len. Visible getattr size stays the frontier until REPORT resolves
 * the rsv. Optional sess_uuid tags the reservation so a later fence
 * resolves it as FENCED_HOLE (10.5c-35d). Absent keeps the zero-UUID
 * stand-in. */
static int host_append_primary(struct efs_raft_host *h, uint8_t group,
                                struct efs_msg_inode_reply *out)
{
    pthread_mutex_lock(&h->mu);
    struct efs_raft *r = group_raft(h, group);
    int primary = r && efs_raft_role(r) == EFS_RAFT_LEADER;
    int hint = r ? efs_raft_leader(r) : -1;
    pthread_mutex_unlock(&h->mu);
    if (!primary) {
        out->status = EFS_INODE_RPC_NOT_PRIMARY;
        out->primary_id = hint >= 0 ? (efs_node_id_t)(hint + 1) : 0;
    }
    return primary;
}

void server_raft_host_append(efs_ino_t ino, uint64_t len,
                             const uint8_t *sess_uuid, uint32_t sess_epoch,
                             uint64_t op_seq, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_stat st;
    uint8_t cmd[HOST_APPEND_RSV_LEN];
    uint32_t clen = 0;
    uint64_t reserved = 0;
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
        host_fwd_append(h, ino, len, sess_uuid, sess_epoch, op_seq, out, &ig, 1);
        return;
    }
    if (!host_append_primary(h, ig, out))
        return;
    if (sess_uuid && op_seq &&
        host_append_replay_get(h, ino, sess_uuid, op_seq, &reserved) == 0) {
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
        set_inode_rc(out, rc, hint);
        if (rc == EFS_OK) {
            stat_to_inode(&st, &out->inode);
            out->inode.size = reserved + len;
            out->inode.atime = reserved;
        }
        return;
    }
    rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && !S_ISREG(row.mode))
        rc = EFS_ERR_INVAL;
    /* Do not session-accept here: leftover-1 FUSE has no session record,
     * and accept-on-miss is BUSY. The suffix is only an appender id so a
     * second client's reserve waits for the first REPORT. */
    if (rc == EFS_OK) {
        uint8_t zu[EFS_OPID_UUID_LEN];
        const uint8_t *uuid = sess_uuid;
        int fr;

        if (!uuid) {
            memset(zu, 0, sizeof(zu));
            uuid = zu;
        }
        fr = efs_meta_apply_append_foreign(h->kv, ino, uuid);
        if (fr > 0)
            rc = EFS_ERR_BUSY;
        else if (fr < 0)
            rc = fr;
    }
    if (rc == EFS_OK)
        rc = pack_append_rsv_cmd(cmd, &clen, ino, len, sess_uuid, sess_epoch,
                                 op_seq);
    if (rc == EFS_OK)
        rc = host_propose_wait_ex(h, ig, cmd, clen, &hint, &reserved);
    if (rc == EFS_OK && sess_uuid && op_seq)
        host_append_replay_put(h, ino, sess_uuid, op_seq, reserved);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    /* Hollow apply is a BUSY retry, not a campaign ban. Marking the
     * handler replica here poisoned the dual-host voter that still had
     * the row: it forwarded to a hollow g2 leader, then disabled itself
     * and the group had zero campaigners. Only host_apply on the leader
     * that actually missed the row may step down. */
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_ERR_BUSY;
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        /* This reservation's end, not getattr's frontier (0 while
         * nopen>0) and not the live watermark. atime is the start so
         * the client can check start+len==size. */
        out->inode.size = reserved + len;
        out->inode.atime = reserved;
    }
}

/* Hard link: dest dentry + inode nlink++ as a txn (same PREPARE/DECIDE/
 * RESOLVE as MKDIR). LOCAL dest stamps the parent row; HASHED/SPLITTING
 * stamp the dir-lane (parent row only for used_shards first-use).
 * SPLITTING dest writes hashed (same as CREATE). HASHED/SPLITTING dest
 * dentries bounce if this replica does not host the dentry shard.
 * Directories are INVAL. LINK_SHARD is not this path. */
void server_raft_host_link(efs_ino_t src_ino, efs_ino_t new_parent,
                           const char *new_name, const struct efs_opid_req *q,
                           struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row dprow, row;
    struct efs_meta_dentry dent, ndent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct efs_txn_ino_delta pd, id;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES];
    uint32_t kd = 0, ki = 0, kp = 0, ks = 0;
    uint32_t dsh, ish, psh, coord;
    uint64_t dver = 0, now;
    int hint = -1;
    int rc, i, touch_parent = 0, stamp_lane = 0;
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
        host_fwd_link(h, src_ino, new_parent, new_name, q, out, need, nn);
        return;
    }
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
        {
            uint32_t li;
            uint64_t lbits = row.active_lanes;

            for (li = 0; li < EFS_META_LANES; li++) {
                if ((lbits & (1ull << li)) == 0)
                    continue;
                bounce_add(gs, &ngs, 8,
                           efs_raft_shard_group(efs_kv_lane_shard(
                               src_ino, (uint8_t)li)));
            }
        }
        if (!host_hosts_all(h, gs, ngs)) {
            uint8_t need[2];

            host_need_both(need);
            host_fwd_link(h, src_ino, new_parent, new_name, q, out, need, 2);
            return;
        }
        if (efs_raft_shard_group(dsh) != efs_raft_shard_group(psh))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    }
    if (rc == EFS_OK) {
        /* I16: the retry of a committed LINK sees its own new name and
         * would get EEXIST. */
        struct efs_opid_reply rep;
        int hit = host_opid_replay(h, dsh, q, &rep);

        if (hit < 0) {
            rc = hit;
        } else if (hit) {
            host_opid_reply_ino(h, new_parent, new_name, &rep, out);
            return;
        }
    }
    if (rc == EFS_OK) {
        rc = efs_meta_apply_lookup_tx(h->kv, new_parent, new_name, host_txn_coord, h, &dent);
        if (rc == EFS_OK) {
            set_inode_rc(out, EFS_ERR_EXIST, hint);
            return;
        }
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
    }
    if (rc == EFS_OK) {
        memset(&ndent, 0, sizeof(ndent));
        ndent.ino = row.ino;
        ndent.generation = row.generation;
        ndent.type = row.mode & S_IFMT;
        /* Source row nlink++ / ctime and the parent's count / times /
         * first-use lane bit are commutative deltas (§7.2). */
        memset(&id, 0, sizeof(id));
        id.d_nlink = 1;
        id.max_ctime = now;
        memset(&pd, 0, sizeof(pd));
        d_lane = dprow.layout == EFS_META_LAYOUT_LOCAL
                     ? 0
                     : efs_kv_dir_lane(new_name);
        if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
            pd.d_nents = 1;
            pd.max_mtime = now;
            pd.max_ctime = now;
            touch_parent = 1;
        } else {
            pd.or_used_shards = 1ull << d_lane;
            if ((dprow.used_shards & pd.or_used_shards) == 0)
                touch_parent = 1;
            stamp_lane = 1;
        }
        rc = efs_meta_pack_dentry(&ndent, v_dent, sizeof(v_dent));
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(dsh, new_parent, new_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, src_ino, k_ino, &ki);
    if (rc == EFS_OK && touch_parent)
        rc = efs_kv_key_inode(psh, new_parent, k_par, &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, new_parent, d_lane, k_dseq, &ks);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK) {
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
                    rc = host_prep_dseq_bump(h, dsh, &t, &parts, k_dseq, ks,
                                             NULL, &hint);
                if (rc == EFS_OK && stamp_lane)
                    rc = host_prep_lane_stamp(h, dsh, &t, &parts, &dprow,
                                              new_name, now, NULL, &hint);
                if (rc == EFS_OK)
                    rc = host_prep_opid(h, dsh, &t, &parts, q, src_ino, 0,
                                        NULL, &hint);
            }
            if (rc == EFS_OK && touch_parent && sh == psh)
                rc = host_prep_ino_delta(h, psh, &t, &parts, k_par, kp, &pd,
                                         NULL, &hint);
            if (rc == EFS_OK && sh == ish)
                rc = host_prep_ino_delta(h, ish, &t, &parts, k_ino, ki, &id,
                                         NULL, &hint);
        }
link_prepped:
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            rc = host_txn_commit(h, &t, &parts, coord, &hint);
        }
    }
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, src_ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, src_ino, host_txn_coord, h, &st);
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
 * GUARDs used-lane dseqs, cap EFS_TXN_NAMESPACE_MAX_PART → BUSY). HASHED dentries
 * and scattered dir inodes bounce if this replica does not host every
 * participant. */
void server_raft_host_rename_at(efs_ino_t old_parent, const char *old_name,
                                efs_ino_t new_parent, const char *new_name,
                                const struct efs_opid_req *q,
                                struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, dprow, row, nrow;
    struct efs_meta_dentry dent, ndent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct host_pver_guard gv[EFS_TXN_NAMESPACE_MAX_PART];
    struct efs_txn_ino_delta pd, dd, id, nd;
    uint8_t k_dst[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_par[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX], k_pver[EFS_KV_KEY_MAX];
    uint8_t k_nino[EFS_KV_KEY_MAX], k_ndseq[EFS_KV_KEY_MAX];
    uint8_t k_dpar[EFS_KV_KEY_MAX], k_ddseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES];
    uint8_t v_pver[8];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint32_t kd = 0, ki = 0, kp = 0, kq = 0, kpv = 0;
    uint32_t kn = 0, knd = 0, krl = 0, kdp = 0, kdsq = 0;
    uint32_t ssh, dsh, ish, psh, dpsh, coord, nsh = 0, ash = 0;
    uint64_t dver = 0, ever = 0;
    uint64_t nver = 0, gver2 = 0, rver = 0;
    uint64_t now;
    int hint = -1;
    int rc, i, is_dir = 0, ngv = 0, nxd = 0, hashed_xdir = 0;
    int xist = 0, xdir = 0, xput = 0, same = 0;
    int touch_src = 0, stamp_src = 0, touch_dst = 0, stamp_dst = 0, two_dseq = 0;
    uint8_t s_lane = 0, d_lane = 0;
    struct host_pver_guard xdseq[EFS_TXN_NAMESPACE_MAX_PART];
    struct efs_meta_dentry_drop drop;
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
    memset(&pd, 0, sizeof(pd));
    memset(&dd, 0, sizeof(dd));
    memset(&id, 0, sizeof(id));
    memset(&nd, 0, sizeof(nd));
    psh = efs_kv_inode_shard(old_parent);
    dpsh = efs_kv_inode_shard(new_parent);
    {
        uint8_t need[2];
        int nn = 1;
        need[0] = efs_raft_shard_group(psh);
        if (efs_raft_shard_group(dpsh) != need[0])
            need[nn++] = efs_raft_shard_group(dpsh);
        if (!host_hosts_all(h, need, nn)) {
            host_fwd_rename(h, old_parent, old_name, new_parent, new_name, q,
                            out, need, nn);
            return;
        }
    }
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
            host_fwd_rename(h, old_parent, old_name, new_parent, new_name, q,
                            out, need, 2);
            return;
        }
    }
    if (rc == EFS_OK && efs_raft_shard_group(ssh) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(ssh), &hint);
    if (rc == EFS_OK && efs_raft_shard_group(dsh) != efs_raft_shard_group(psh) &&
        efs_raft_shard_group(dsh) != efs_raft_shard_group(ssh) &&
        efs_raft_shard_group(dsh) != efs_raft_shard_group(dpsh))
        rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    if (rc == EFS_OK) {
        /* I16: the retry of a committed rename finds old_name gone
         * (ENOENT) or new_name present. The window is on the dest dentry
         * shard. */
        struct efs_opid_reply rep;
        int hit = host_opid_replay(h, dsh, q, &rep);

        if (hit < 0) {
            rc = hit;
        } else if (hit) {
            host_opid_reply_ino(h, new_parent, new_name, &rep, out);
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup_tx(h->kv, old_parent, old_name, host_txn_coord, h, &dent);
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
        int xrc = efs_meta_apply_lookup_tx(h->kv, new_parent, new_name, host_txn_coord, h, &ndent);
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
        /* A DIRECTORY rename GUARDs the dest's whole ancestry chain
         * (host_pver_guard_chain: every ancestor's parent_version up to
         * the root). Each MKDIR scatters its inode, so the chain spans
         * both groups almost always, and the walk reads those rows from
         * the local KV — on a single-group host the first foreign ancestor
         * is group_raft()==NULL → NOT_PRIMARY with no leader hint, which
         * the client cannot follow (EIO on `mv dir`; deterministic while
         * the old_parent group's leader is a single-group node, Sep 20).
         * Only a dual host can run a dir rename. */
        if (is_dir) {
            bounce_add(gs, &ngs, 8, EFS_RAFT_GROUP_SHARD);
            bounce_add(gs, &ngs, 8, EFS_RAFT_GROUP_SHARD2);
        }
        all = 1;
        for (j = 0; j < ngs; j++)
            if (!host_hosts(h, gs[j]))
                all = 0;
        if (!all) {
            uint8_t need[2];
            need[0] = EFS_RAFT_GROUP_SHARD;
            need[1] = EFS_RAFT_GROUP_SHARD2;
            host_fwd_rename(h, old_parent, old_name, new_parent, new_name, q,
                            out, need, 2);
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_resolve_tx(h->kv, old_parent, old_name, host_txn_coord, h, &dent, &row);
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
    if (rc == EFS_OK && !host_row_lanes_hosted(h, row.ino, &row)) {
        uint8_t need[2];

        host_need_both(need);
        host_fwd_rename(h, old_parent, old_name, new_parent, new_name, q,
                        out, need, 2);
        return;
    }
    if (rc == EFS_OK && xist) {
        rc = efs_meta_apply_get_inode(h->kv, ndent.ino, &nrow);
        /* The dest dentry was here; an unlink of it committing between
         * that read and this one is the dest moving under the txn.
         * NOT_FOUND would be ENOENT for a rename whose source is alive
         * (peer_rename_vs_unlink_dst). STALE makes the client re-run. */
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_ERR_STALE;
    }
    if (rc == EFS_OK && xist && !host_row_lanes_hosted(h, ndent.ino, &nrow)) {
        uint8_t need[2];

        host_need_both(need);
        host_fwd_rename(h, old_parent, old_name, new_parent, new_name, q,
                        out, need, 2);
        return;
    }
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
                host_fwd_rename(h, old_parent, old_name, new_parent, new_name,
                                q, out, need, 2);
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
                if (nxd >= EFS_TXN_NAMESPACE_MAX_PART) {
                    rc = EFS_ERR_BUSY;
                    break;
                }
                lsh = efs_kv_lane_shard(nrow.ino, (uint8_t)lane);
                xdseq[nxd].shard = lsh;
                xdseq[nxd].klen = 0;
                rc = efs_kv_key_dseq(lsh, nrow.ino, (uint8_t)lane, xdseq[nxd].key,
                                      &xdseq[nxd].klen);
                if (rc == EFS_OK)
                    rc = efs_txn_dseq_observe(h->kv, xdseq[nxd].key,
                                              xdseq[nxd].klen, &xdseq[nxd].ver);
                if (rc == EFS_OK)
                    nxd++;
            }
        }
        if (rc == EFS_OK && (same ? prow.nlink : dprow.nlink) < 3)
            rc = EFS_ERR_PROTO;
        if (rc == EFS_OK) {
            /* the replaced empty dir leaves its parent: nlink-- there */
            if (same)
                pd.d_nlink--;
            else
                dd.d_nlink--;
        }
    }
    /* Every row this rename touches is a commutative delta (§7.2): the
     * two parents (pd / dd), the moved inode (id: parent SET, ctime,
     * parent_version++), a replaced dest file (nd: nlink--). */
    if (rc == EFS_OK && xist && !xdir) {
        memset(&nd, 0, sizeof(nd));
        nd.d_nlink = -1;
        nd.max_ctime = now;
        if (nrow.nlink <= 1) {
            int held = efs_lease_any(h->kv, nrow.ino, nrow.generation);
            if (held < 0)
                rc = held;
            else if (held)
                xput = 1;
        } else
            xput = 1;
    }
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
            pd.d_nlink--;
            dd.d_nlink++;
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
            pd.max_mtime = now;
            pd.max_ctime = now;
            if (!(same && !xist))
                pd.d_nents--;
            touch_src = 1;
        } else {
            bit = 1ull << s_lane;
            pd.or_used_shards |= bit;
            if ((prow.used_shards & bit) == 0)
                touch_src = 1;
            stamp_src = 1;
        }
        if (!same) {
            if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
                dd.max_mtime = now;
                dd.max_ctime = now;
                if (!xist)
                    dd.d_nents++;
                touch_dst = 1;
            } else {
                bit = 1ull << d_lane;
                dd.or_used_shards |= bit;
                if ((dprow.used_shards & bit) == 0)
                    touch_dst = 1;
                stamp_dst = 1;
            }
        } else if (prow.layout != EFS_META_LAYOUT_LOCAL) {
            bit = 1ull << d_lane;
            pd.or_used_shards |= bit;
            if ((prow.used_shards & bit) == 0)
                touch_src = 1;
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
        if (rc == EFS_OK) {
            row.parent_version++;
            id.d_pver = 1;
        }
    }
    if (rc == EFS_OK) {
        memset(&ndent, 0, sizeof(ndent));
        ndent.ino = row.ino;
        ndent.generation = row.generation;
        ndent.type = row.mode & S_IFMT;
        id.set_parent = new_parent;
        id.max_ctime = now;
        rc = efs_meta_pack_dentry(&ndent, v_dent, sizeof(v_dent));
    }
    if (rc == EFS_OK)
        rc = efs_meta_capture_dentry_drop(h->kv, &prow, old_parent, old_name, &dent, &drop);
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
    if (rc == EFS_OK && xist && !xput)
        rc = efs_txn_ver_get(h->kv, k_nino, kn, &nver);
    if (rc == EFS_OK && xist && xdir && !hashed_xdir)
        rc = efs_txn_dseq_observe(h->kv, k_ndseq, knd, &gver2);
    if (rc == EFS_OK && xist && !xdir && !xput)
        rc = efs_txn_ver_get(h->kv, k_reap, krl, &rver);
    if (rc == EFS_OK) {
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
                    rc = host_prep_dseq_bump(h, ssh, &t, &parts, k_dseq, kq,
                                             NULL, &hint);
                if (rc == EFS_OK && stamp_src)
                    rc = host_prep_lane_stamp(h, ssh, &t, &parts, &prow,
                                              old_name, now, NULL, &hint);
                if (rc == EFS_OK && stamp_dst && dsh == ssh)
                    rc = host_prep_lane_stamp(h, ssh, &t, &parts,
                                              same ? &prow : &dprow, new_name,
                                              now, NULL, &hint);
                if (rc == EFS_OK && two_dseq && dsh == ssh)
                    rc = host_prep_dseq_bump(h, ssh, &t, &parts, k_ddseq, kdsq,
                                             NULL, &hint);
                if (rc == EFS_OK && dsh == ssh)
                    rc = host_prep_opid(h, dsh, &t, &parts, q, row.ino, 0,
                                        NULL, &hint);
            }
            if (rc == EFS_OK && sh == dsh && dsh != ssh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dst, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               &hint);
                if (rc == EFS_OK && two_dseq)
                    rc = host_prep_dseq_bump(h, dsh, &t, &parts, k_ddseq, kdsq,
                                             NULL, &hint);
                if (rc == EFS_OK && stamp_dst)
                    rc = host_prep_lane_stamp(h, dsh, &t, &parts,
                                              same ? &prow : &dprow, new_name,
                                              now, NULL, &hint);
                if (rc == EFS_OK)
                    rc = host_prep_opid(h, dsh, &t, &parts, q, row.ino, 0,
                                        NULL, &hint);
            }
            if (rc == EFS_OK && touch_src && sh == psh)
                rc = host_prep_ino_delta(h, psh, &t, &parts, k_par, kp, &pd,
                                         NULL, &hint);
            if (rc == EFS_OK && touch_dst && sh == dpsh)
                rc = host_prep_ino_delta(h, dpsh, &t, &parts, k_dpar, kdp, &dd,
                                         NULL, &hint);
            if (rc == EFS_OK && sh == ish) {
                rc = host_prep_ino_delta(h, ish, &t, &parts, k_ino, ki, &id,
                                         NULL, &hint);
                if (rc == EFS_OK && is_dir)
                    rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_pver,
                                   kpv, ever, EFS_TXN_PUT, v_pver, 8, &hint);
            }
            if (rc == EFS_OK && xist && sh == nsh) {
                if (xput)
                    rc = host_prep_ino_delta(h, nsh, &t, &parts, k_nino, kn,
                                             &nd, NULL, &hint);
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
            rc = host_txn_commit(h, &t, &parts, coord, &hint);
        }
    }
    /* Only a commit that returned OK may be answered OK when the row is
     * gone. A NOT_FOUND from the commit itself is the loser of
     * peer_rename_same_src_two_dst (source already moved); turning that
     * into OK made both renames report success. */
    {
        int committed = (rc == EFS_OK);

        if (rc == EFS_OK)
            rc = host_read_inode_lanes(h, row.ino, &hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_getattr(h->kv, row.ino, host_txn_coord, h, &st);
        if (committed && rc == EFS_ERR_NOT_FOUND) {
            memset(&st, 0, sizeof(st));
            st.ino = row.ino;
            rc = EFS_OK;
        }
    }
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = new_parent;
        strncpy(out->inode.name, new_name, EFS_MAX_NAME - 1);
    }
}

/* OPEN reservations whose range is at or below the published size become
 * COMPLETED. No new opcode: REPORT already committed the data. Catch-up
 * with nopen==0 is a no-op (existing raft-smoke-p). */
static int host_resolve_caught_up(struct efs_raft_host *h, efs_ino_t ino,
                                  uint64_t sz, int *hint)
{
    uint64_t offs[64], lens[64];
    uint32_t n = 64, i, clen = 0;
    uint8_t cmd[HOST_APPEND_RES_LEN];
    uint8_t ig;
    int rc;

    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    /* append_open caps at 64. A 400-record O_APPEND burst is 400 OPEN
     * rows; one pass left nopen>0 and the peer stayed BUSY forever. */
    for (;;) {
        uint32_t did = 0;

        n = 64;
        rc = efs_meta_apply_append_open(h->kv, ino, offs, lens, &n);
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_OK;
        if (rc != EFS_OK)
            return rc;
        if (n == 0)
            return EFS_OK;
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
            did++;
        }
        if (!did)
            return EFS_OK;
    }
}

/* Files and symlinks carry a chunk map; FUSE stores a symlink target as
 * ordinary published bytes. Directories do not. */
static int host_holds_chunks(uint32_t mode)
{
    return S_ISREG(mode) || S_ISLNK(mode);
}

/* One chunk CAS + lane MAX. First-use of a lane whose
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

/* Pack one PUBLISH. *clen=0 means skip (identical mapping or deleted).
 * *lg is the raft group that must host the entry. skip_ri is for the
 * REPORT batch path (ReadIndex already done per group). cached is the
 * inode row from the report's one get_inode; NULL falls back to a get. */
static int host_pub_pack(struct efs_raft_host *h, const struct efs_chunk_rec *rec,
                         uint64_t new_size, int *hint, int skip_ri,
                         struct efs_meta_row *cached,
                         uint8_t *cmd, uint32_t *clen, uint8_t *lg_out)
{
    struct efs_meta_pub p;
    struct efs_meta_row row;
    struct efs_meta_chunk got;
    uint32_t lsh, ish;
    uint8_t lane, lg, ig;
    int rc, i;

    *clen = 0;
    if (!rec || rec->ino == 0)
        return EFS_ERR_INVAL;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (rec->nodes[i] == 0) {
            if (raft_dbg_on())
                fprintf(stderr, "raft-host: pub ino=%llu ci=%u INVAL node0\n",
                        (unsigned long long)rec->ino, rec->chunk_index);
            return EFS_ERR_INVAL;
        }
    }
    ish = efs_kv_inode_shard(rec->ino);
    ig = efs_raft_shard_group(ish);
    rc = skip_ri ? EFS_OK : host_read_index(h, ig, hint);
    if (rc != EFS_OK)
        return rc;
    if (cached) {
        row = *cached;
        rc = EFS_OK;
    } else {
        rc = efs_meta_apply_get_inode(h->kv, rec->ino, &row);
    }
    /* Stale publish for a deleted inode: skip the proposal entirely (P3).
     * ReadIndex guarantees we see the committed create, so NOT_FOUND here
     * means the unlink already won. */
    if (rc == EFS_ERR_NOT_FOUND) {
        if (rec->publish_flags & EFS_CHUNK_REC_F_CAPTURED_FILEID)
            return EFS_ERR_STALE;
        if (raft_dbg_on())
            fprintf(stderr, "raft-host: pub ino=%llu ci=%u stale (deleted), skip\n",
                    (unsigned long long)rec->ino, rec->chunk_index);
        return EFS_OK;
    }
    if (rc != EFS_OK) {
        if (raft_dbg_on())
            fprintf(stderr, "raft-host: pub ino=%llu ci=%u get_inode rc=%d\n",
                    (unsigned long long)rec->ino, rec->chunk_index, rc);
        return rc;
    }
    if (!host_holds_chunks(row.mode)) {
        if (raft_dbg_on())
            fprintf(stderr, "raft-host: pub ino=%llu ci=%u INVAL mode=%o\n",
                    (unsigned long long)rec->ino, rec->chunk_index, row.mode);
        return EFS_ERR_INVAL;
    }
    lane = (uint8_t)(rec->chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(rec->ino, lane);
    lg = efs_raft_shard_group(lsh);
    if (lg_out)
        *lg_out = lg;
    /* A fold names the authority epoch of the bytes it actually fetched.
     * Never re-stamp an old materialization with a later truncation epoch. */
    uint64_t publish_epoch;
    int epoch_rc = efs_chunk_publish_authority(rec, row.generation, row.content_epoch, &publish_epoch);
    if (epoch_rc != EFS_OK)
        return epoch_rc;
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
        if (cached)
            cached->active_lanes |= 1ULL << lane;
    }
    if (lg != ig && !skip_ri)
        rc = host_read_index(h, lg, hint);
    if (rc != EFS_OK)
        return rc;
    memset(&got, 0, sizeof(got));
    /* First publish (base_gen=0): apply CAS(expected=0) is the existence
     * check. A per-rec lsm_get here was 65536 lookups on an 8 GiB fsync
     * and never skipped (skip-identical needs a live mapping).
     * A span publish is not the base image, so the identical-fragment
     * skip does not apply to it. */
    if (rec->delta_len) {
        uint64_t cur = 0;

        if (rec->base_gen == EFS_CHUNK_BASE_UNCOND)
            return EFS_ERR_INVAL;
        /* A span that is already in the chain is a retry of a REPORT
         * whose apply wait timed out (BUSY) after the entry was proposed.
         * The apply would answer OK for it; proposing it again costs a
         * second 2048-pub entry per batch on every replica. 36 ranks
         * fsyncing one file re-proposed 10072 spans per client per
         * retry (Sep 30 IO-500, `report-split nrec=10072 … rc=-13` ×13
         * per client) and the apply backlog never drained. One get. */
        rc = efs_meta_apply_chunk_holds(h->kv, rec->ino, row.generation,
                                        rec->chunk_index,
                                        rec->chunk_generation,
                                        rec->delta_off, rec->delta_len,
                                        &cur);
        if (rc == 1)
            return EFS_OK;
        if (rc != 0 && rc != EFS_ERR_NOT_FOUND)
            return rc;
        if (rec->base_gen != 0) {
            if (rc == EFS_ERR_NOT_FOUND)
                return EFS_ERR_STALE;
            if (cur != rec->base_gen)
                return EFS_ERR_STALE;
        }
        rc = EFS_OK;
    } else if (rec->base_gen == 0) {
        /* Same retry for a first full image: the committed generation
         * IS our object. `got` stays zero — expected_gen stays 0. */
        rc = efs_meta_apply_chunk_holds(h->kv, rec->ino, row.generation,
                                        rec->chunk_index,
                                        rec->chunk_generation, 0, 0, NULL);
        if (rc == 1)
            return EFS_OK;
        if (rc != 0 && rc != EFS_ERR_NOT_FOUND)
            return rc;
        rc = EFS_OK;
    } else if (rec->base_gen != 0) {
        rc = efs_meta_apply_get_chunk(h->kv, rec->ino, rec->chunk_index, &got);
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
        else if (rc != EFS_OK)
            return rc;
        if (got.generation != 0 &&
            (!(rec->publish_flags & EFS_CHUNK_REC_F_FRESH_OBJECT) ||
             got.generation == rec->chunk_generation) &&
            memcmp(got.nodes, rec->nodes, sizeof(got.nodes)) == 0 &&
            memcmp(got.checksums, rec->checksums, sizeof(got.checksums)) == 0)
            return EFS_OK;
        if (rec->base_gen != EFS_CHUNK_BASE_UNCOND &&
            rec->base_gen != got.generation)
            return EFS_ERR_STALE;
    }
    memset(&p, 0, sizeof(p));
    p.ino = rec->ino;
    p.chunk_index = rec->chunk_index;
    p.new_size = new_size;
    p.now = now_ns();
    p.expected_gen = (rec->base_gen == EFS_CHUNK_BASE_UNCOND)
                         ? got.generation
                         : rec->base_gen;
    /* Prefer the PUT object name from the client. Hashing here is only
     * a last resort — a hash of empty checksums is a constant that was
     * never written (remount GET DECODE). */
    p.candidate_gen = rec->chunk_generation;
    p.fresh_object = !!(rec->publish_flags & EFS_CHUNK_REC_F_FRESH_OBJECT);
    if (!p.candidate_gen)
        p.candidate_gen = host_pub_candidate_gen(rec, rec->chunk_index);
    if (p.candidate_gen == 0)
        p.candidate_gen = 1;
    p.content_epoch = publish_epoch;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    memcpy(p.ch.nodes, rec->nodes, sizeof(p.ch.nodes));
    memcpy(p.ch.checksums, rec->checksums, sizeof(p.ch.checksums));
    p.delta_off = rec->delta_off;
    p.delta_len = rec->delta_len;
    p.delta_base_n = rec->delta_base_n;
    p.delta_base_seq = rec->delta_base_seq;
    if (rec->publish_flags & EFS_CHUNK_REC_F_CAPTURED_FILEID)
        p.inode_gen = rec->file_generation;
    if (lg != ig) {
        /* Lane-local: the lane group's KV has no inode row. The FileID
         * fields ride the entry (read above under ReadIndex on the inode
         * group); the lane's own fenced_epoch is the truncate fence. */
        p.inode_gen = row.generation;
        p.mtime_gen = row.mtime_gen;
        p.lane_local = 1;
    }
    return pack_publish_cmd(cmd, clen, &p);
}

/* One REPORT's publications for one Raft group. A report larger than
 * HOST_PUB_BATCH_N pubs is proposed as SEVERAL log entries (push proposes
 * when the buffer fills, finish proposes the tail). Every entry's apply
 * verdict matters: a pub that loses its N-1 CAS at apply time (a peer's
 * publish landed between our pack and our apply) is STALE on the ring for
 * ITS entry only. Checking just the last entry — what this did until Sep 20
 * — returned OK for a report whose first entry held STALE losers, the
 * client marked those ranges clean, and the winner's generation (merged on
 * an older base) became the file: ior-hard 36 ranks × 3000 segs lost 6707
 * of 108000 records, every one "everything one client wrote into a shared
 * chunk", zeros in a cold verify. `idxs` remembers every proposed index;
 * `overflow` (alloc failure) makes the wait answer STALE, never OK. */
struct host_pub_batch {
    uint8_t group;
    uint32_t len;
    uint8_t *buf;
    uint64_t last_idx;
    uint64_t t0; /* us, set when the last entry of this batch was proposed */
    uint64_t *idxs;
    uint64_t *terms; /* per idxs[i]; 0 = forwarded, index-only match */
    uint32_t nidx, cidx;
    int overflow;
};

static void host_pub_batch_reset(struct host_pub_batch *b)
{
    free(b->buf);
    free(b->idxs);
    free(b->terms);
    b->buf = NULL;
    b->idxs = NULL;
    b->terms = NULL;
    b->len = 0;
    b->last_idx = 0;
    b->nidx = b->cidx = 0;
    b->overflow = 0;
}

static int host_pub_batch_propose(struct efs_raft_host *h, struct host_pub_batch *b,
                                  int *hint)
{
    uint64_t idx = 0, term = 0;
    int rc;

    if (!b->buf || !b->len)
        return EFS_OK;
    /* L0 is within one flush of the cap. BUSY here, on the leader,
     * instead of the apply path waiting out a compaction. */
    if (h->kv && efs_kv_lsm_l0_hot(h->kv))
        return EFS_ERR_BUSY;
    b->t0 = now_us_();
    rc = host_propose(h, b->group, b->buf, b->len, &idx, &term, hint);
    b->len = 0;
    if (rc != EFS_OK)
        return rc;
    b->last_idx = idx;
    if (b->nidx == b->cidx) {
        uint32_t nc = b->cidx ? b->cidx * 2 : 8;
        uint64_t *ni = realloc(b->idxs, (size_t)nc * sizeof(*ni));
        uint64_t *nt;

        if (!ni) {
            b->overflow = 1;
            return EFS_OK;
        }
        b->idxs = ni;
        nt = realloc(b->terms, (size_t)nc * sizeof(*nt));
        if (!nt) {
            b->overflow = 1;
            return EFS_OK;
        }
        b->terms = nt;
        b->cidx = nc;
    }
    b->idxs[b->nidx] = idx;
    b->terms[b->nidx] = term;
    b->nidx++;
    return EFS_OK;
}

static int host_pub_batch_push(struct efs_raft_host *h, struct host_pub_batch *b,
                               uint8_t group, const uint8_t *cmd, uint32_t clen,
                               int *hint)
{
    int rc;

    if (clen == 0)
        return EFS_OK;
    if (!b->buf) {
        b->buf = malloc((size_t)HOST_PUBLISH_LEN * HOST_PUB_BATCH_N);
        if (!b->buf)
            return EFS_ERR_NOMEM;
        b->len = 0;
        b->group = group;
        b->last_idx = 0;
    }
    if (b->len && b->group != group)
        return EFS_ERR_INVAL;
    if (b->len + clen > HOST_PUBLISH_LEN * HOST_PUB_BATCH_N) {
        rc = host_pub_batch_propose(h, b, hint);
        if (rc != EFS_OK)
            return rc;
    }
    b->group = group;
    memcpy(b->buf + b->len, cmd, clen);
    b->len += clen;
    return EFS_OK;
}

static int host_pub_batch_finish(struct efs_raft_host *h, struct host_pub_batch *b,
                                 int *hint)
{
    return host_pub_batch_propose(h, b, hint);
}

/* Wait for the LAST proposed entry (indices in one group are monotonic, so
 * every earlier one is applied too), then read EVERY entry's verdict. A
 * ring miss (entry evicted from the HOST_APPLY_RC_RING before we looked,
 * or `overflow`) is STALE here, not OK: the client's STALE path re-pulls
 * the map and replays only chunks whose generation moved (a no-op when
 * nothing did), whereas an unfounded OK drops dirty ranges for good. The
 * first non-OK verdict wins, except that any STALE is reported as STALE
 * (the client can only repair STALE by retrying). */
static int host_pub_batch_wait(struct efs_raft_host *h, struct host_pub_batch *b,
                               int *hint)
{
    int rc = EFS_OK, saw_stale = 0;
    uint64_t idx = b->last_idx;
    uint32_t i;

    if (idx)
        rc = host_wait_applied(h, b->group, idx, hint);
    if (rc == EFS_OK && idx) {
        struct host_group *g;

        pthread_mutex_lock(&h->mu);
        g = group_slot(h, b->group);
        if (g && !g->hosted)
            g = NULL; /* forwarded batch: the leader's reply was the verdict */
        for (i = 0; i < b->nidx && g; i++) {
            uint64_t s = b->idxs[i] & HOST_APPLY_RC_MASK;
            int erc;

            if (g->arc_idx[s] != b->idxs[i]) {
                h->obs_arc_miss++;
                erc = EFS_ERR_STALE;
            } else if (b->terms[i] && g->arc_term[s] != b->terms[i]) {
                /* Our entry was truncated by a leader change; a stranger
                 * applied at this index. The publish did not happen:
                 * STALE makes the client re-pull and replay the range. */
                h->obs_arc_term_miss++;
                erc = EFS_ERR_STALE;
            } else {
                erc = g->arc_rc[s];
            }
            if (erc == EFS_ERR_STALE)
                saw_stale = 1;
            else if (erc != EFS_OK && rc == EFS_OK)
                rc = erc;
        }
        pthread_mutex_unlock(&h->mu);
        /* !g (group not hosted) cannot happen here: the report path
         * requires every group it publishes to be local (checked above). */
        if (b->overflow)
            saw_stale = 1;
        if (rc == EFS_OK && saw_stale)
            rc = EFS_ERR_STALE;
    }
    if (idx && b->t0) {
        uint64_t dt = now_us_() - b->t0;
        uint64_t n = __atomic_fetch_add(&h->obs_pub_n, 1, __ATOMIC_RELAXED);
        uint64_t mx;

        __atomic_store_n(&h->obs_pub_ring[n % 64], dt, __ATOMIC_RELAXED);
        mx = __atomic_load_n(&h->obs_pub_max_us, __ATOMIC_RELAXED);
        while (dt > mx &&
               !__atomic_compare_exchange_n(&h->obs_pub_max_us, &mx, dt,
                                            0, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED))
            ;
    }
    host_pub_batch_reset(b);
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
    if (raft_dbg_on())
        fprintf(stderr, "raft-host: report count=%u ino_count=%u\n",
                count, ino_count);
    /* The batch can touch both groups (per-rec inode group + lane group).
     * Every group it needs must be local — a publish proposed to a group
     * this node doesn't vote in could not be waited on, and its apply on
     * a voter lacking the inode row would diverge. Bounce the whole batch
     * to a dual-host (never self, so no forward loop). */
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
    /* One ReadIndex per touched group for the whole batch (§8 amortize).
     * A per-rec ReadIndex is a quorum round each and times out a 2g
     * end_fsync (16k chunks) as EFS_ERR_NET. */
    for (i = 0; i < (uint32_t)nn && rc == EFS_OK; i++)
        rc = host_read_index(h, need[i], &hint);
    {
        efs_ino_t uino[32];
        uint64_t usz[32];
        uint32_t nu = 0, k;
        struct host_pub_batch bat[HOST_NGROUPS];
        uint8_t cmd[HOST_PUBLISH_LEN];
        uint32_t clen;
        uint8_t lg;
        int bi;
        struct {
            efs_ino_t ino;
            struct efs_meta_row row;
            uint64_t need_act;
            int have;
        } ic[32];
        uint32_t nic = 0;
        uint64_t t_pack = 0, t_push = 0, t0, t_fin0, t_fin1;
        uint32_t n_skip = 0, n_stale = 0, n_inval = 0;
        int held = 0;
        int had_stale = 0;
        const char *fail_phase = "-";
        int fail_rc = 0;

        memset(bat, 0, sizeof(bat));
        memset(ic, 0, sizeof(ic));
        /* One get_inode per ino, then one ACTIVATE_LANE mask per ino
         * (was up to 64 propose_wait rounds on a new file). */
        for (i = 0; i < count && rc == EFS_OK; i++) {
            uint8_t lane, ig, lgg;
            uint32_t ish, lsh;

            for (k = 0; k < nic; k++)
                if (ic[k].ino == recs[i].ino)
                    break;
            if (k == nic) {
                if (nic >= 32)
                    continue;
                ic[k].ino = recs[i].ino;
                rc = efs_meta_apply_get_inode(h->kv, recs[i].ino, &ic[k].row);
                if (rc == EFS_ERR_NOT_FOUND) {
                    if (recs[i].publish_flags & EFS_CHUNK_REC_F_CAPTURED_FILEID) {
                        rc = EFS_ERR_STALE;
                        break;
                    }
                    rc = EFS_OK;
                    continue;
                }
                if (rc != EFS_OK)
                    break;
                ic[k].have = 1;
                nic++;
            }
            if (!ic[k].have)
                continue;
            uint64_t captured_epoch;
            rc = efs_chunk_publish_authority(&recs[i], ic[k].row.generation,
                                              ic[k].row.content_epoch, &captured_epoch);
            if (rc != EFS_OK)
                break;
            lane = (uint8_t)(recs[i].chunk_index % EFS_META_LANES);
            ish = efs_kv_inode_shard(recs[i].ino);
            lsh = efs_kv_lane_shard(recs[i].ino, lane);
            ig = efs_raft_shard_group(ish);
            lgg = efs_raft_shard_group(lsh);
            if (lgg != ig && (ic[k].row.active_lanes & (1ULL << lane)) == 0) {
                ic[k].need_act |= 1ULL << lane;
                ic[k].row.active_lanes |= 1ULL << lane;
            }
        }
        for (k = 0; k < nic && rc == EFS_OK; k++) {
            uint8_t acmd[HOST_ACTIVATE_MASK_LEN];
            uint32_t alen = 0;
            uint8_t ig;

            if (!ic[k].need_act)
                continue;
            ig = efs_raft_shard_group(efs_kv_inode_shard(ic[k].ino));
            rc = pack_activate_mask_cmd(acmd, &alen, ic[k].ino, ic[k].need_act);
            if (rc == EFS_OK)
                rc = host_propose_wait(h, ig, acmd, alen, &hint);
        }
        if (rc != EFS_OK) {
            fail_phase = "act";
            fail_rc = rc;
        }
        if (h->disk && efs_raft_disk_sync_hold(h->disk) == EFS_OK)
            held = 1;
        for (i = 0; i < count && rc == EFS_OK; i++) {
            struct efs_meta_row *cached = NULL;

            sz = 0;
            clen = 0;
            lg = 0;
            for (j = 0; j < ino_count && irecs; j++) {
                if (irecs[j].ino == recs[i].ino) {
                    sz = irecs[j].size;
                    break;
                }
            }
            if (sz == 0)
                sz = ((uint64_t)recs[i].chunk_index + 1) * EFS_MIN_CHUNK_SIZE;
            for (k = 0; k < nic; k++) {
                if (ic[k].have && ic[k].ino == recs[i].ino) {
                    cached = &ic[k].row;
                    break;
                }
            }
            t0 = now_us_();
            rc = host_pub_pack(h, &recs[i], sz, &hint, 1, cached, cmd, &clen,
                               &lg);
            t_pack += now_us_() - t0;
            if (raft_dbg_on())
                fprintf(stderr,
                        "raft-host: report pub ino=%llu ci=%u sz=%llu rc=%d clen=%u\n",
                        (unsigned long long)recs[i].ino, recs[i].chunk_index,
                        (unsigned long long)sz, rc, clen);
            /* INVAL is a permanent property of the rec (bad mode, zero node) —
             * it can never succeed, so skip it and keep publishing the rest of
             * the batch. One poison rec must not stall every rec behind it
             * (the client re-reports the skipped ino, so a misdiagnosed cause
             * resurfaces instead of being lost). Transient errors (NOT_PRIMARY,
             * NO_QUORUM, NET, BUSY) fail the batch for a client retry. */
            if (rc == EFS_ERR_INVAL) {
                fprintf(stderr,
                        "raft-host: report pub ino=%llu ci=%u INVAL, skipping rec\n",
                        (unsigned long long)recs[i].ino, recs[i].chunk_index);
                n_inval++;
                rc = EFS_OK;
                continue;
            }
            /* STALE is different again: a losing CAS on ONE rec must not
             * abort the whole batch. The prefix-before-the-first-STALE
             * already committed (the batch finish/wait below runs on any
             * rc), so aborting here threw away nothing but forced the
             * client to resend everything after the loser too — and at
             * 36 ranks on one shared file the multi-minute batch window
             * made a fresh conflict per round near-certain (livelock:
             * 2h18m without a clean pass). Skip the loser, publish the
             * rest, and return STALE at the end so the client retries
             * only the residual (its re-pull now replays just the moved
             * chunks). */
            if (rc == EFS_ERR_STALE) {
                if (raft_dbg_on())
                    fprintf(stderr,
                            "raft-host: report pub ino=%llu ci=%u STALE, skipping rec\n",
                            (unsigned long long)recs[i].ino,
                            recs[i].chunk_index);
                had_stale = 1;
                n_stale++;
                rc = EFS_OK;
                continue;
            }
            if (rc == EFS_OK && clen == 0)
                n_skip++; /* pack: already committed, nothing proposed */
            if (rc == EFS_OK && clen > 0) {
                for (bi = 0; bi < HOST_NGROUPS; bi++) {
                    if (!bat[bi].buf || bat[bi].group == lg)
                        break;
                }
                if (bi == HOST_NGROUPS) {
                    rc = EFS_ERR_INVAL;
                } else {
                    t0 = now_us_();
                    rc = host_pub_batch_push(h, &bat[bi], lg, cmd, clen, &hint);
                    t_push += now_us_() - t0;
                }
            }
            if (rc == EFS_OK) {
                for (k = 0; k < nu; k++)
                    if (uino[k] == recs[i].ino)
                        break;
                if (k == nu && nu < 32) {
                    uino[nu] = recs[i].ino;
                    usz[nu] = sz;
                    nu++;
                } else if (k < nu && sz > usz[k]) {
                    usz[k] = sz;
                }
            }
            if (rc != EFS_OK) {
                fail_phase = clen ? "push" : "pack";
                fail_rc = rc;
                if (dirop_fail_on(rc))
                    fprintf(stderr,
                            "raft-host: report FAIL ino=%llu ci=%u rc=%d (phase=%s)\n",
                            (unsigned long long)recs[i].ino, recs[i].chunk_index, rc,
                            "pub");
            }
        }
        t_fin0 = now_us_();
        for (bi = 0; bi < HOST_NGROUPS; bi++) {
            int frc = host_pub_batch_finish(h, &bat[bi], &hint);

            if (rc == EFS_OK && frc != EFS_OK) {
                fail_phase = "finish";
                fail_rc = frc;
            }
            if (rc == EFS_OK)
                rc = frc;
        }
        if (held)
            (void)efs_raft_disk_sync_release(h->disk);
        /* The hold suppressed the per-entry fsync, and the durable ceiling
         * kept those entries out of AppendEntries. They are on disk now. */
        pthread_mutex_lock(&h->mu);
        for (bi = 0; bi < HOST_NGROUPS; bi++) {
            struct efs_raft *rr;

            if (!bat[bi].last_idx)
                continue;
            rr = group_raft(h, bat[bi].group);
            if (rr && efs_raft_role(rr) == EFS_RAFT_LEADER)
                (void)efs_raft_durable(rr, bat[bi].last_idx);
        }
        pthread_mutex_unlock(&h->mu);
        host_pump_kick(h);
        for (bi = 0; bi < HOST_NGROUPS; bi++) {
            int wrc = host_pub_batch_wait(h, &bat[bi], &hint);

            if (rc == EFS_OK && wrc != EFS_OK) {
                fail_phase = "wait";
                fail_rc = wrc;
            }
            if (rc == EFS_OK)
                rc = wrc;
            else
                host_pub_batch_reset(&bat[bi]);
        }
        t_fin1 = now_us_();
        /* Committed everything committable; tell the client to retry the
         * skipped losers (its residual report re-pulls and replays just
         * those). The resolve pass below stays gated on a fully clean
         * round, matching the old whole-batch STALE behavior. */
        if (rc == EFS_OK && had_stale)
            rc = EFS_ERR_STALE;
        if (count >= 256u || n_inval ||
            (t_pack + t_push + (t_fin1 - t_fin0)) >= 100000ull)
            fprintf(stderr,
                    "report-split nrec=%u pack_ms=%llu push_ms=%llu finish_ms=%llu skip=%u stale=%u inval=%u rc=%d fail=%s/%d\n",
                    (unsigned)count,
                    (unsigned long long)(t_pack / 1000ull),
                    (unsigned long long)(t_push / 1000ull),
                    (unsigned long long)((t_fin1 - t_fin0) / 1000ull),
                    n_skip, n_stale, n_inval, rc, fail_phase, fail_rc);
        /* Deleted inode is already OK (skipped above). A NOT_FOUND
         * that remains is a log index the snapshot removed under the
         * propose. Map it to BUSY so the report is retried. The log
         * line above still shows the original rc. A NOT_FOUND that
         * reached the client was the 9-client fsync EIO. */
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_ERR_BUSY;
        /* Once per inode, not per chunk: append_open is a KV scan, and a 2g
         * file is 16k recs. Doing it per rec times out the client as NET. */
        for (k = 0; k < nu && rc == EFS_OK; k++) {
            int rrc = host_resolve_caught_up(h, uino[k], usz[k], &hint);

            if (rrc != EFS_OK)
                fprintf(stderr,
                        "raft-host: report resolve ino=%llu rc=%d (deferred)\n",
                        (unsigned long long)uino[k], rrc);
        }
    }
    set_inode_rc(out, rc, hint);
}

/* Staged endpoint: one immutable intent, one durable outcome. No aggregate
 * reply or transient apply-result ring is used as evidence of ownership. */
void server_raft_host_publication(const struct efs_msg_publication *req, int query_only,
                                  struct efs_msg_publication_reply *out)
{
    struct efs_raft_host *h=g_host;struct efs_meta_pub p;int hint=-1,verdict;
    memset(out,0,sizeof(*out));out->rpc.status=EFS_INODE_RPC_INVAL;
    if(query_only<0 || query_only>2 || !h || !h->running || !req ||
       efs_publication_from_rec(&req->rec,req->size,&p)!=EFS_OK)return;
    p.publication_id=req->id;
    if(efs_publication_digest(&p,out->digest)!=EFS_OK)return;
    uint32_t shard=efs_kv_lane_shard(p.ino,p.chunk_index%EFS_META_LANES);
    uint8_t group=efs_raft_shard_group(shard);
    if(!host_hosts(h,group)) {
        int skip=-1;out->rpc.status=EFS_INODE_RPC_NOT_PRIMARY;
        for(int tries=0;tries<h->n;tries++) {
            int rid=host_pick_peer(h,&group,1,skip);if(rid<0)return;
            if(host_inode_rpc_peer(h,rid,query_only==2?EFS_MSG_PUBLICATION_RETIRE:query_only?EFS_MSG_PUBLICATION_STATUS:EFS_MSG_PUBLICATION,
               req,sizeof(*req),query_only==2?EFS_MSG_PUBLICATION_RETIRE_REPLY:query_only?EFS_MSG_PUBLICATION_STATUS_REPLY:EFS_MSG_PUBLICATION_REPLY,
               out,sizeof(*out))==0 && out->rpc.status!=EFS_INODE_RPC_NOT_PRIMARY)return;
            memset(out,0,sizeof(*out));out->rpc.status=EFS_INODE_RPC_NOT_PRIMARY;skip=rid;
        }
        return;
    }
    int rc=host_read_index(h,group,&hint);
    if(rc==EFS_OK)rc=efs_session_accept(h->kv,shard,req->id.client_uuid,req->id.session_epoch);
    if(rc==EFS_OK)rc=efs_meta_publication_result(h->kv,&p,&verdict);
    if((rc==EFS_ERR_NOT_FOUND && !query_only) ||
       (rc==EFS_OK && query_only==2 && verdict!=EFS_META_PUBLICATION_RETIRED)) {
        /* Apply validates FileID/fences again. No pack-time CAS skip: losers
         * must have a replicated terminal result, and retries use that result. */
        p.now=now_ns();p.durable_result=1;
        p.lane_local=group!=efs_raft_shard_group(efs_kv_inode_shard(p.ino));
        uint8_t cmd[HOST_PUBLICATION_LEN];uint32_t len=0;
        rc=pack_publish_cmd(cmd,&len,&p);
        if(rc==EFS_OK) {
            cmd[0]=query_only==2?EFS_MD_CMD_PUBLICATION_RETIRE:EFS_MD_CMD_PUBLICATION;
            memcpy(cmd+len,p.publication_id.client_uuid,EFS_OPID_UUID_LEN);
            wr32be(cmd+len+16,p.publication_id.session_epoch);wr64be(cmd+len+20,p.publication_id.seq);
            len+=28;rc=host_propose_wait(h,group,cmd,len,&hint);
        }
        /* Even STALE may describe another entry or a lost apply-ring result.
         * Resolve this intent from durable state after an authoritative read. */
        if(rc==EFS_OK || rc==EFS_ERR_STALE || rc==EFS_ERR_BUSY) {
            rc=host_read_index(h,group,&hint);
            if(rc==EFS_OK)rc=efs_session_accept(h->kv,shard,req->id.client_uuid,req->id.session_epoch);
            if(rc==EFS_OK)rc=efs_meta_publication_result(h->kv,&p,&verdict);
        }
    }
    if(rc==EFS_OK && query_only==2 && verdict!=EFS_META_PUBLICATION_RETIRED)rc=EFS_ERR_BUSY;
    set_inode_rc(&out->rpc,rc,hint);
    if(rc==EFS_OK) {
        out->verdict=verdict;
        out->state=verdict==EFS_META_PUBLICATION_RETIRED?EFS_PUBLICATION_RETIRED:
            verdict==EFS_OK?EFS_PUBLICATION_COMMITTED:EFS_PUBLICATION_REJECTED;
    }
}

void server_raft_host_lane_bootstrap_rpc(const struct efs_msg_lane_bootstrap *req,
                                          struct efs_msg_lane_writer_view_reply *out)
{
    struct efs_raft_host *h = g_host;
    memset(out, 0, sizeof(*out));
    out->view.status = EFS_INODE_RPC_INVAL;
    if (!h || !h->running || !req->ino || !req->generation ||
        !efs_chunk_size_valid(req->chunk_size))
        return;
    uint8_t groups[2] = {efs_raft_shard_group(efs_kv_inode_shard(req->ino)),
        efs_raft_shard_group(efs_kv_lane_shard(req->ino, req->chunk_index % EFS_META_LANES))};
    int ng = groups[0] == groups[1] ? 1 : 2;
    if (!host_hosts(h, groups[0]) || !host_hosts(h, groups[ng - 1])) {
        int skip = -1;
        out->view.status = EFS_INODE_RPC_NOT_PRIMARY;
        for (int i = 0; i < h->n; ++i) {
            int rid = host_pick_peer(h, groups, ng, skip);
            if (rid < 0)
                return;
            if (!host_inode_rpc_peer(h, rid, EFS_MSG_LANE_BOOTSTRAP, req, sizeof(*req),
                  EFS_MSG_LANE_BOOTSTRAP_REPLY, out, sizeof(*out)) &&
                out->view.status != EFS_INODE_RPC_NOT_PRIMARY)
                return;
            memset(out, 0, sizeof(*out));
            out->view.status = EFS_INODE_RPC_NOT_PRIMARY;
            skip = rid;
        }
        return;
    }
    pthread_mutex_lock(&h->s->lock);
    struct efs_export *ex = server_export_acquire_locked(h->s, req->export_id);
    uint32_t cs = ex ? server_data_chunk_size(ex) : 0;
    pthread_mutex_unlock(&h->s->lock);
    if (!ex)
        return;
    server_export_put(h->s, ex);
    if (cs != req->chunk_size) {
        out->view.status = EFS_INODE_RPC_STALE;
        return;
    }
    int hint = -1;
    int rc = server_raft_host_lane_bootstrap(req->ino, req->generation,
                                              req->chunk_index, cs, &hint);
    if (rc != EFS_OK) {
        out->view.status = rc_to_inode_status(rc);
        out->view.primary_id = hint >= 0 ? (efs_node_id_t)(hint + 1) : 0;
        return;
    }
    struct efs_msg_lane_writer_view q = {req->ino, req->generation, req->chunk_index, cs};
    server_raft_host_lane_writer_view(&q, out);
}

void server_raft_host_lane_writer_view(const struct efs_msg_lane_writer_view *req,
                                        struct efs_msg_lane_writer_view_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_writer_view view;
    int hint = -1;
    memset(out, 0, sizeof(*out));
    out->view.status = EFS_INODE_RPC_INVAL;
    if (!h || !h->running || !req->ino || !req->generation ||
        !efs_chunk_size_valid(req->chunk_size))
        return;
    uint8_t group = efs_raft_shard_group(efs_kv_lane_shard(req->ino,
                                       req->chunk_index % EFS_META_LANES));
    /* A topology with no dual-host replicas still has a valid ordinary path. */
    if (!host_hosts(h, group)) {
        int skip = -1;
        out->view.status = EFS_INODE_RPC_NOT_PRIMARY;
        for (int tries = 0; tries < h->n; ++tries) {
            int rid = host_pick_peer(h, &group, 1, skip);
            if (rid < 0)
                return;
            if (host_inode_rpc_peer(h, rid, EFS_MSG_LANE_WRITER_VIEW, req, sizeof(*req),
                  EFS_MSG_LANE_WRITER_VIEW_REPLY, out, sizeof(*out)) == 0 &&
                out->view.status != EFS_INODE_RPC_NOT_PRIMARY)
                return;
            memset(out, 0, sizeof(*out));
            out->view.status = EFS_INODE_RPC_NOT_PRIMARY;
            skip = rid;
        }
        return;
    }
    int rc = host_read_index(h, group, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_get_lane_writer_view(h->kv, req->ino, req->generation,
                                            req->chunk_index, req->chunk_size, &view);
    out->view.status = rc_to_inode_status(rc);
    out->view.primary_id = hint >= 0 ? (efs_node_id_t)(hint + 1) : 0;
    if (rc == EFS_OK) {
        out->view.ino = view.ino; out->view.generation = view.generation;
        out->view.chunk_index = req->chunk_index;
        out->view.authority_epoch = view.authority_epoch;
        out->view.oldest_complete_epoch = view.oldest_complete_epoch;
        out->view.history = view.history; out->chunk_size = req->chunk_size;
    }
}

void server_raft_host_writer_view(const struct efs_msg_inode_writer_view *req,
                                   struct efs_msg_inode_writer_view_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_writer_view view;
    int hint = -1, rc;
    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_INVAL;
    if (!h || !h->running || !req->ino || req->reserved)
        return;
    uint8_t groups[2] = {
        efs_raft_shard_group(efs_kv_inode_shard(req->ino)),
        efs_raft_shard_group(efs_kv_lane_shard(req->ino,
                              req->chunk_index % EFS_META_LANES))};
    int ng = groups[0] == groups[1] ? 1 : 2;
    if (!host_hosts(h, groups[0]) || !host_hosts(h, groups[ng - 1])) {
        int skip = -1;
        out->status = EFS_INODE_RPC_NOT_PRIMARY;
        for (int tries = 0; tries < h->n; ++tries) {
            int rid = host_pick_peer(h, groups, ng, skip);
            if (rid < 0)
                return;
            if (host_inode_rpc_peer(h, rid, EFS_MSG_INODE_WRITER_VIEW, req,
                  sizeof(*req), EFS_MSG_INODE_WRITER_VIEW_REPLY, out, sizeof(*out)) == 0 &&
                out->status != EFS_INODE_RPC_NOT_PRIMARY)
                return;
            memset(out, 0, sizeof(*out));
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            skip = rid;
        }
        return;
    }
    rc = host_read_index(h, groups[0], &hint);
    if (rc == EFS_OK && ng == 2)
        rc = host_read_index(h, groups[1], &hint);
    uint64_t gen = req->generation;
    if (rc == EFS_OK && !gen) {
        struct efs_meta_row row;
        rc = efs_meta_apply_get_inode_tx(h->kv, req->ino, host_txn_coord, h, &row);
        if (rc == EFS_OK)
            gen = row.generation;
    }
    if (rc == EFS_OK)
        rc = efs_meta_get_writer_chunk_view_tx(h->kv, req->ino, gen,
                    req->chunk_index, host_txn_coord, h, &view);
    out->status = rc_to_inode_status(rc);
    out->primary_id = hint >= 0 ? (efs_node_id_t)(hint + 1) : 0;
    if (rc == EFS_OK) {
        out->ino = view.ino;
        out->generation = view.generation;
        out->chunk_index = req->chunk_index;
        out->authority_epoch = view.authority_epoch;
        out->oldest_complete_epoch = view.oldest_complete_epoch;
        out->history = view.history;
    }
}

void server_raft_host_getchunks(efs_export_id_t export_id, efs_ino_t ino, uint32_t start, uint32_t max, uint64_t generation,
                                struct efs_msg_inode_getchunks_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_chunk ch;
    uint32_t ci, group_end, lsh, seen = 0;
    uint32_t cs = EFS_DEFAULT_CHUNK_SIZE;
    uint8_t ig, lg;
    int hint = -1;
    int rc;
    uint64_t t0 = now_ns(), t_ri = 0, t1;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    pthread_mutex_lock(&h->s->lock);
    struct efs_export *ex = server_export_acquire_locked(h->s, export_id);
    if (ex)
        cs = server_data_chunk_size(ex);
    pthread_mutex_unlock(&h->s->lock);
    if (ex)
        server_export_put(h->s, ex);
    if (max == 0 || max > EFS_GETCHUNKS_MAX)
        max = EFS_GETCHUNKS_MAX;
    group_end = (start | (EFS_CHUNK_GROUP_SIZE - 1u)) + 1u;
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, ig)) {
        uint8_t need[2];
        host_need_both(need);
        host_fwd_getchunks(h, export_id, ino, start, max, generation, out, need, 2);
        return;
    }
    rc = host_read_index(h, ig, &hint);
    t_ri = now_ns() - t0;
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode_tx(h->kv, ino, host_txn_coord, h, &row);
    if (rc == EFS_OK && !host_holds_chunks(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && generation && row.generation != generation)
        rc = EFS_ERR_STALE;
    if (rc == EFS_OK) {
        out->ino = ino;
        out->generation = row.generation;
        out->authority_epoch = row.content_epoch;
    }
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
            host_fwd_getchunks(h, export_id, ino, start, max, generation, out, need, nn);
            return;
        }
        if ((seen & (1u << lg)) == 0) {
            int hh = -1;
            uint64_t tr = now_ns();

            rc = host_read_index(h, lg, &hh);
            t_ri += now_ns() - tr;
            if (rc != EFS_OK)
                hint = hh;
            else
                seen |= 1u << lg;
        }
        if (rc != EFS_OK)
            break;
        {
            struct efs_meta_chunk_view view;
            struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
            uint32_t nd = 0, di;
            uint64_t newest = 0;

            /* One get: image and span trailer together. */
            rc = efs_meta_get_chunk_view_tx(h->kv, ino, row.generation,
                                            ci, cs, host_txn_coord, h, &view);
            if (rc == EFS_ERR_NOT_FOUND) {
                rc = EFS_OK;
                continue;
            }
            if (rc != EFS_OK)
                break;
            ch = view.base;
            nd = view.ndelta;
            newest = view.delta_seq;
            memcpy(ds, view.deltas, nd * sizeof(*ds));
            out->recs[out->count].read_view = view.bytes;
            out->recs[out->count].ino = ino;
            out->recs[out->count].chunk_index = ci;
            memcpy(out->recs[out->count].nodes, ch.nodes,
                   sizeof(out->recs[out->count].nodes));
            memcpy(out->recs[out->count].checksums, ch.checksums,
                   sizeof(out->recs[out->count].checksums));
            out->recs[out->count].base_gen = ch.generation;
            out->recs[out->count].chunk_generation = ch.generation;
            out->recs[out->count].delta_off = 0;
            out->recs[out->count].delta_len = 0;
            if (nd > 0) {
                if (nd > EFS_CHUNK_DELTA_MAX)
                    nd = EFS_CHUNK_DELTA_MAX;
                out->recs[out->count].delta_base_n = nd;
                out->recs[out->count].delta_base_seq = newest;
                for (di = 0; di < nd; di++) {
                    out->recs[out->count].deltas[di].off = ds[di].off;
                    out->recs[out->count].deltas[di].len = ds[di].len;
                    out->recs[out->count].deltas[di].generation =
                        ds[di].generation;
                    out->recs[out->count].deltas[di].seq = ds[di].seq;
                    memcpy(out->recs[out->count].deltas[di].nodes,
                           ds[di].nodes, sizeof(ds[di].nodes));
                    memcpy(out->recs[out->count].deltas[di].checksums,
                           ds[di].checksums, sizeof(ds[di].checksums));
                }
            }
        }
        out->count++;
    }
    out->status = rc_to_inode_status(rc);
    out->primary_id = (hint >= 0) ? (efs_node_id_t)(hint + 1) : 0;
    t1 = now_ns();
    if (t1 - t0 > 20000000ull) {
        static uint64_t last_ns, slow;

        slow++;
        if (t1 - last_ns > 1000000000ull) {
            last_ns = t1;
            fprintf(stderr,
                    "raft-host: getchunks slow ino=%llu start=%u n=%u "
                    "ri_ms=%llu total_ms=%llu rc=%d slow=%llu\n",
                    (unsigned long long)ino, start, out->count,
                    (unsigned long long)(t_ri / 1000000ull),
                    (unsigned long long)((t1 - t0) / 1000000ull), rc,
                    (unsigned long long)slow);
        }
    }
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
            /* Readdir is a weak name listing (spec §7.4): ino + type +
             * name. A getattr + ReadIndex per child made `ls` of an
             * 81-name ROOT take 1.3s and the posix leftover sweep miss
             * its SSH budget. Size/nlink come from LOOKUP/GETATTR. */
            memset(&out->ents[out->count], 0, sizeof(out->ents[0]));
            out->ents[out->count].ino = page[i].d.ino;
            out->ents[out->count].mode = page[i].d.type;
            out->ents[out->count].parent = parent;
            strncpy(out->ents[out->count].name, page[i].name,
                    EFS_MAX_NAME - 1);
            out->count++;
        }
    }
    /* Resume cookie: the cursor sits past the last emitted entry (or past
     * the last scanned one when a filter dropped it — either way nothing is
     * re-scanned or skipped). */
    out->next_src = cur.src;
    out->next_done = cur.done ? 1 : 0;
    strncpy(out->next_name, cur.name, EFS_MAX_NAME - 1);
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
    rc = host_read_inode_lanes(h, cur, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        uint8_t need[2];

        host_need_both(need);
        host_fwd_lookup_path(h, start, path, out, need, 2);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, cur, &row);
    if (rc == EFS_OK && *p == '\0') {
        rc = efs_meta_apply_getattr(h->kv, cur, host_txn_coord, h, &st);
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
            host_fwd_lookup_path(h, start, path, out, need, nn);
            return;
        }
        if (dg != pg)
            rc = host_read_index(h, dg, &hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_lookup_tx(h->kv, cur, name, host_txn_coord, h, &dent);
        if (rc == EFS_OK) {
            uint8_t cg = efs_raft_shard_group(efs_kv_inode_shard(dent.ino));
            if (!host_hosts(h, cg)) {
                uint8_t need[2];
                int nn = 1;
                need[0] = pg;
                if (cg != pg)
                    need[nn++] = cg;
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

            host_need_both(need);
            host_fwd_lookup_path(h, start, path, out, need, 2);
            return;
        }
        if (rc == EFS_OK)
            rc = efs_meta_apply_getattr(h->kv, cur, host_txn_coord, h, &st);
    }
    set_inode_rc((struct efs_msg_inode_reply *)out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = last_parent;
        strncpy(out->inode.name, last_name, EFS_MAX_NAME - 1);
    }
}
