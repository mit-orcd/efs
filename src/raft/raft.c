#include "efs/raft.h"
#include "efs/wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct efs_raft {
    int id;
    int role;
    int leader;
    uint8_t group;
    uint64_t boot_id;
    uint64_t current_term;
    int32_t voted_for;
    uint64_t commit_index;
    uint64_t last_applied;
    uint64_t snap_idx;
    uint64_t snap_term;
    uint64_t next_index[EFS_RAFT_MAX_PEERS];
    uint64_t match_index[EFS_RAFT_MAX_PEERS];
    /* Catch-up flow control: first index of the entry batch last sent to
     * this peer and not yet answered (0 = none outstanding). While set,
     * propose/commit-driven broadcasts send this peer NOTHING — the batch
     * already carries every index it can take, and a duplicate 128-entry
     * batch per propose was what buried a behind follower (fcstor005 Sep 19:
     * the same 128-entry window rewritten + fsynced ~30×/s, 128 entries of
     * real progress per ~10 s). A batch older than heartbeat_ticks is
     * retransmitted by the next send (tick heartbeat or propose), so a lost
     * batch or reply still recovers within one heartbeat interval —
     * broadcast_ae resets hb_elapsed, so under load the tick heartbeat
     * itself never fires and cannot be the only retransmit path.
     * ae_inflight_commit is the leader_commit that batch carried. A later
     * commit (the other peer acked) is not in it; suppressing every send
     * then hid the new commit until the 50 ms heartbeat. A moved commit
     * sends an empty probe (no entry rewrite — that was the Sep 19 storm)
     * marked vote_granted so the reply does not drop this batch. */
    uint64_t ae_inflight[EFS_RAFT_MAX_PEERS];
    uint64_t ae_inflight_tick[EFS_RAFT_MAX_PEERS];
    uint64_t ae_inflight_commit[EFS_RAFT_MAX_PEERS];
    uint64_t ae_inflight_end[EFS_RAFT_MAX_PEERS]; /* last index that batch carried */
    uint64_t ticks; /* efs_raft_tick count, for ae_inflight aging */
    uint64_t peer_boot[EFS_RAFT_MAX_PEERS];
    unsigned vote_bits;
    uint32_t election_elapsed;
    uint32_t election_ticks;
    uint32_t election_deadline; /* randomized in [election_ticks, 2*election_ticks) */
    uint64_t rng;               /* election-timeout PRNG state (seeded via cfg) */
    uint32_t hb_elapsed;
    uint32_t heartbeat_ticks;
    uint64_t read_index;
    unsigned read_acks;
    int read_in_flight;
    uint32_t log_old;
    uint32_t log_new;
    uint32_t app_old;
    uint32_t app_new;
    uint32_t learners;
    uint64_t joint_idx;
    uint64_t cold_idx;
    struct efs_raft_store *store;
    void *store_ctx;
    efs_raft_send_fn send;
    void *net;
    efs_raft_apply_fn apply;
    void *app;
    efs_raft_snap_open_fn snap_open;
    efs_raft_snap_read_fn snap_read;
    efs_raft_snap_close_fn snap_close;
    efs_raft_snap_chunk_fn snap_chunk;
    uint32_t snap_chunk_bytes;
    void *snap_handle;
    uint64_t snap_total;
    uint64_t snap_handle_incl;
    uint64_t snap_off[EFS_RAFT_MAX_PEERS];
    uint64_t snap_peer_incl[EFS_RAFT_MAX_PEERS];
    /* No-progress InstallSnapshot ack: do not read another chunk until
     * this tick. Propose wakes were copying the same chunk every time
     * the follower answered BUSY. An empty AppendEntries still goes out. */
    uint64_t snap_retry_tick[EFS_RAFT_MAX_PEERS];
    /* Follower staging cursor. Offset 0 restarts it. */
    uint64_t rx_incl;
    uint64_t rx_term;
    uint64_t rx_off;
    uint32_t rx_old;
    uint32_t rx_new;
    int allow_campaign; /* 0 = do not start elections (hollow KV) */
    /* send_idx may go out in AppendEntries before the leader fsync.
     * durable_idx is what this leader has fsynced; try_commit must not
     * count the leader's own match above it (a follower majority is
     * still a real majority). */
    int ae_capped;
    uint64_t send_idx;
    uint64_t durable_idx;
};

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr64(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int popc(uint32_t x)
{
    int n = 0;
    while (x) {
        n += (int)(x & 1u);
        x >>= 1;
    }
    return n;
}

static int majority(uint32_t set, unsigned bits)
{
    int need = popc(set) / 2 + 1;
    return popc(set & (uint32_t)bits) >= need;
}

static int quorum_ok(const struct efs_raft *r, unsigned bits)
{
    if (!majority(r->log_old, bits))
        return 0;
    if (r->log_new && !majority(r->log_new, bits))
        return 0;
    return 1;
}

static int solo(const struct efs_raft *r)
{
    return popc(r->log_old) == 1 && r->log_new == 0;
}

static int is_voter(const struct efs_raft *r, int id)
{
    uint32_t bit;

    if (id < 0 || id >= EFS_RAFT_MAX_PEERS)
        return 0;
    bit = 1u << id;
    if (r->log_new)
        return (r->log_old & bit) || (r->log_new & bit);
    return (r->log_old & bit) != 0;
}

static uint32_t peer_mask(const struct efs_raft *r)
{
    /* Include the applied config so a COLD entry still reaches members
     * who are leaving: otherwise they campaign on C_old and livelock
     * elections (I18 disruption). */
    return r->log_old | r->log_new | r->learners | r->app_old | r->app_new;
}

static int in_app_cfg(const struct efs_raft *r, int id)
{
    uint32_t bit;

    if (id < 0 || id >= EFS_RAFT_MAX_PEERS)
        return 0;
    bit = 1u << id;
    if (r->app_new)
        return (r->app_old & bit) || (r->app_new & bit);
    return (r->app_old & bit) != 0;
}

static int is_cfg_cmd(const uint8_t *cmd, uint32_t clen)
{
    if (!cmd || clen < 5)
        return 0;
    if (cmd[0] == EFS_RAFT_CMD_JOINT)
        return clen >= 9;
    if (cmd[0] == EFS_RAFT_CMD_COLD)
        return 1;
    return 0;
}

static int save_hard(struct efs_raft *r)
{
    return r->store->save_hard(r->store_ctx, r->current_term, r->voted_for);
}

static int save_cfg(struct efs_raft *r)
{
    if (!r->store->save_cfg)
        return EFS_OK;
    return r->store->save_cfg(r->store_ctx, r->app_old, r->app_new);
}

static int last_log(struct efs_raft *r, uint64_t *index, uint64_t *term)
{
    return r->store->last(r->store_ctx, index, term);
}

static int log_term(struct efs_raft *r, uint64_t index, uint64_t *term)
{
    uint32_t clen = 0;
    int rc;

    if (index == 0) {
        *term = 0;
        return EFS_OK;
    }
    rc = r->store->get(r->store_ctx, index, term, NULL, &clen);
    if (rc == EFS_ERR_INVAL)
        return EFS_OK;
    return rc;
}

static void install_log_cfg(struct efs_raft *r, const uint8_t *cmd,
                            uint32_t clen, uint64_t idx)
{
    uint32_t a, b;

    if (!is_cfg_cmd(cmd, clen))
        return;
    a = rd32(cmd + 1);
    if (cmd[0] == EFS_RAFT_CMD_JOINT) {
        b = rd32(cmd + 5);
        r->log_old = a;
        r->log_new = b;
        r->joint_idx = idx;
        r->learners &= ~(a | b);
    } else {
        r->log_old = a;
        r->log_new = 0;
        r->cold_idx = idx;
        r->learners &= ~a;
    }
}

static int reload_cfg_from_log(struct efs_raft *r)
{
    uint64_t last_i = 0, last_t = 0, idx;

    r->log_old = r->app_old;
    r->log_new = r->app_new;
    r->joint_idx = 0;
    r->cold_idx = 0;
    if (last_log(r, &last_i, &last_t) != EFS_OK)
        return EFS_ERR_IO;
    for (idx = r->snap_idx + 1; idx <= last_i; idx++) {
        uint8_t buf[16];
        uint32_t clen = sizeof(buf);
        uint64_t term = 0;
        int rc = r->store->get(r->store_ctx, idx, &term, buf, &clen);
        if (rc == EFS_ERR_INVAL)
            continue;
        if (rc != EFS_OK)
            return rc;
        install_log_cfg(r, buf, clen, idx);
    }
    return EFS_OK;
}

static int send_msg(struct efs_raft *r, struct efs_raft_msg *m)
{
    if (!r->send)
        return EFS_ERR_INVAL;
    m->from = r->id;
    m->term = r->current_term;
    m->group = r->group;
    m->boot_id = r->boot_id;
    return r->send(r->net, m);
}

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Reset the election timer and redraw the deadline uniformly from
 * [election_ticks, 2*election_ticks). The randomization is what guarantees
 * election liveness: with a fixed timeout, synchronized followers campaign at
 * the same term, vote for themselves, and split the quorum indefinitely — and
 * a behind-log candidate with a shorter timeout can perpetually reset an
 * up-to-date follower's timer via maybe_step_down, starving it forever.
 * Seeded per-instance (cfg.rng_seed) so the deterministic simulator replays. */
static void reset_election(struct efs_raft *r)
{
    r->election_elapsed = 0;
    r->election_deadline =
        r->election_ticks + (uint32_t)(splitmix64(&r->rng) % r->election_ticks);
}

static int maybe_step_down(struct efs_raft *r, uint64_t term)
{
    if (term <= r->current_term)
        return 0;
    r->current_term = term;
    r->voted_for = -1;
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = -1;
    r->vote_bits = 0;
    r->read_in_flight = 0;
    reset_election(r);
    save_hard(r);
    return 1;
}

static void maybe_leave(struct efs_raft *r)
{
    if (r->role == EFS_RAFT_LEADER && !in_app_cfg(r, r->id)) {
        r->role = EFS_RAFT_FOLLOWER;
        r->leader = -1;
        r->vote_bits = 0;
    }
}

static int apply_committed(struct efs_raft *r)
{
    while (r->last_applied < r->commit_index) {
        uint64_t idx = r->last_applied + 1;
        uint64_t term = 0;
        uint32_t clen = 0;
        uint8_t *cmd = NULL;
        int rc;

        if (idx <= r->snap_idx) {
            r->last_applied = r->snap_idx;
            continue;
        }
        rc = r->store->get(r->store_ctx, idx, &term, NULL, &clen);
        if (rc == EFS_ERR_INVAL) {
            cmd = clen ? malloc(clen) : NULL;
            if (clen && !cmd)
                return EFS_ERR_NOMEM;
            rc = r->store->get(r->store_ctx, idx, &term, cmd, &clen);
        }
        if (rc != EFS_OK) {
            free(cmd);
            return rc;
        }
        if (is_cfg_cmd(cmd, clen)) {
            if (cmd[0] == EFS_RAFT_CMD_JOINT) {
                r->app_old = rd32(cmd + 1);
                r->app_new = rd32(cmd + 5);
            } else {
                r->app_old = rd32(cmd + 1);
                r->app_new = 0;
            }
            save_cfg(r);
            maybe_leave(r);
        } else if (r->apply && clen > 0) {
            rc = r->apply(r->app, idx, term, cmd, clen);
            if (rc != EFS_OK) {
                free(cmd);
                return rc;
            }
        }
        free(cmd);
        r->last_applied = idx;
    }
    if (r->read_in_flight && r->last_applied >= r->read_index &&
        quorum_ok(r, r->read_acks))
        r->read_in_flight = 2;
    return EFS_OK;
}

static int append_local(struct efs_raft *r, uint64_t term, const uint8_t *cmd,
                        uint32_t clen, uint64_t *index_out)
{
    uint64_t last_i = 0, last_t = 0;
    uint64_t idx;
    int rc;

    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    idx = last_i + 1;
    rc = r->store->append(r->store_ctx, idx, term, cmd, clen);
    if (rc != EFS_OK)
        return rc;
    r->match_index[r->id] = idx;
    install_log_cfg(r, cmd, clen, idx);
    if (index_out)
        *index_out = idx;
    return EFS_OK;
}

static void ae_inflight_set(struct efs_raft *r, int to, uint64_t ni,
                            uint64_t end);

static uint32_t snap_chunk_cap(const struct efs_raft *r)
{
    uint32_t n = r->snap_chunk_bytes ? r->snap_chunk_bytes : EFS_RAFT_SNAP_CHUNK;

    if (n > EFS_RAFT_SNAP_CHUNK)
        n = EFS_RAFT_SNAP_CHUNK;
    if (n < 8)
        n = 8;
    return n;
}

/* Open the snapshot for incl. BUSY = export not readable yet (caller
 * retries). NOT_FOUND = this index has no bytes; the caller retries at
 * last_applied (a restarted leader whose file is gone). */
static int snap_ensure(struct efs_raft *r, uint64_t incl)
{
    void *h = NULL;
    uint64_t total = 0;
    int rc;

    if (r->snap_handle && r->snap_handle_incl == incl)
        return EFS_OK;
    if (!r->snap_open) {
        if (r->snap_handle && r->snap_close)
            r->snap_close(r->snap_handle);
        r->snap_handle = NULL;
        r->snap_total = 0;
        r->snap_handle_incl = incl;
        return EFS_OK;
    }
    rc = r->snap_open(r->app, incl, &h, &total);
    if (rc != EFS_OK)
        return rc;
    if (r->snap_handle && r->snap_close)
        r->snap_close(r->snap_handle);
    r->snap_handle = h;
    r->snap_total = total;
    r->snap_handle_incl = incl;
    return EFS_OK;
}

/* One InstallSnapshot chunk. One chunk in flight per peer: the caller
 * sets ae_inflight, and the SNAP_REP advances snap_off. Chunk 0 carries
 * the 8-byte config prefix. EFS_ERR_AGAIN = nothing sent, retry later. */
static int send_snap(struct efs_raft *r, int to)
{
    struct efs_raft_msg m;
    uint8_t *pay = NULL;
    uint64_t incl, incl_t, off, total;
    uint32_t chunk, user_room, user_n, plen, got = 0;
    int done, rc;

    if (r->snap_idx == 0)
        return EFS_OK;
    incl = r->snap_idx;
    incl_t = r->snap_term;
    rc = snap_ensure(r, incl);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_AGAIN) {
        /* The stored snap point has no file (process restart). Re-export
         * at the current applied index, which is exactly the state the
         * caller holds the SM lock over. */
        incl = r->last_applied;
        rc = log_term(r, incl, &incl_t);
        /* The applied index was compacted between the two lookups.
         * AGAIN, not NOT_FOUND: the caller sends an empty AppendEntries
         * and retries. NOT_FOUND here used to fail the proposer's RPC,
         * and a client fsync mapped that to EIO. */
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_ERR_AGAIN;
        if (rc != EFS_OK)
            return rc;
        rc = snap_ensure(r, incl);
    }
    if (rc == EFS_ERR_BUSY)
        return EFS_ERR_AGAIN;
    if (rc != EFS_OK)
        return rc;
    if (r->snap_peer_incl[to] != incl) {
        r->snap_off[to] = 0;
        r->snap_peer_incl[to] = incl;
        r->snap_retry_tick[to] = 0;
    }
    /* Follower still importing this offset. Skip the pread; the caller
     * sends an empty AppendEntries so the election timer stays quiet. */
    if (r->ticks < r->snap_retry_tick[to])
        return EFS_ERR_AGAIN;
    off = r->snap_off[to];
    total = r->snap_total;
    if (off > total)
        off = 0;
    chunk = snap_chunk_cap(r);
    user_room = (off == 0 && chunk > 8) ? chunk - 8 : (off == 0 ? 0 : chunk);
    if (off >= total)
        user_n = 0;
    else if ((uint64_t)user_room > total - off)
        user_n = (uint32_t)(total - off);
    else
        user_n = user_room;
    done = off + user_n >= total;
    plen = user_n + (off == 0 ? 8u : 0u);
    pay = malloc(plen ? plen : 1);
    if (!pay)
        return EFS_ERR_NOMEM;
    if (off == 0) {
        wr32(pay, r->app_old);
        wr32(pay + 4, r->app_new);
    }
    if (user_n) {
        if (!r->snap_read) {
            free(pay);
            return EFS_ERR_INVAL;
        }
        rc = r->snap_read(r->snap_handle, off, pay + (off == 0 ? 8u : 0u),
                          user_n, &got);
        if (rc != EFS_OK || got != user_n) {
            free(pay);
            return rc != EFS_OK ? rc : EFS_ERR_IO;
        }
    }
    if (getenv("EFS_RAFT_DBG"))
        fprintf(stderr, "raft[%u]: send_snap incl=%llu off=%llu n=%u done=%d "
                "to=%d\n", r->group, (unsigned long long)incl,
                (unsigned long long)off, user_n, done, to);
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_SNAP_REQ;
    m.to = to;
    m.last_log_index = incl;
    m.last_log_term = incl_t;
    m.prev_index = off;
    m.success = done ? 1 : 0;
    m.leader_commit = r->commit_index;
    m.nentries = 1;
    m.entries[0].term = incl_t;
    m.entries[0].clen = plen;
    m.entries[0].cmd = pay;
    rc = send_msg(r, &m);
    free(pay);
    if (rc == EFS_OK)
        /* End is not next_index. ae_inflight_fresh treats a batch as
         * stale when send_idx moves past its end, so the next flush
         * resends. A snapshot chunk's bytes do not change when a later
         * entry is proposed, and send_idx is already past next_index
         * for a peer in the snap window — that resend fired on every
         * pump wake (outbox hi=2048, ~20k duplicate chunks/s, the
         * follower's part file advancing ~10/s). The reply or one
         * heartbeat retransmits. UINT64_MAX keeps the send_idx check
         * from matching. */
        ae_inflight_set(r, to, r->next_index[to] ? r->next_index[to] : 1,
                        UINT64_MAX);
    /* Not queued. The next tick retries the same offset; marking this
     * chunk in flight would suppress that retry for a heartbeat. */
    if (rc == EFS_ERR_AGAIN)
        return EFS_OK;
    return rc;
}

/* 1 if a catch-up send starting at ni to `to` is still outstanding and
 * younger than one heartbeat interval — skip; the reply or the age will
 * trigger the next send. */
static int ae_inflight_fresh(const struct efs_raft *r, int to, uint64_t ni)
{
    if (r->ae_inflight[to] != ni)
        return 0;
    if (r->ticks - r->ae_inflight_tick[to] >= (uint64_t)r->heartbeat_ticks)
        return 0;
    /* The outstanding batch does not cover entries appended since it
     * was built. Waiting out the heartbeat (50 ms) left apply one
     * index behind while every raft ACK was still ~10 us. Resend from
     * the same next_index with the longer end; that is a replacement
     * batch, not a second window. */
    if (r->send_idx > r->ae_inflight_end[to])
        return 0;
    return 1;
}

static void ae_inflight_set(struct efs_raft *r, int to, uint64_t ni,
                            uint64_t end)
{
    r->ae_inflight[to] = ni;
    r->ae_inflight_tick[to] = r->ticks;
    r->ae_inflight_commit[to] = r->commit_index;
    r->ae_inflight_end[to] = end;
}

/* Follower already has the outstanding batch (this is queued behind that
 * RPC). Tell it the commit index moved, without resending the entries. */
static int send_commit_probe(struct efs_raft *r, int to)
{
    struct efs_raft_msg m;
    uint64_t end, pt = 0;
    int rc;

    end = r->ae_inflight_end[to];
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_AE_REQ;
    m.to = to;
    m.vote_granted = 1; /* reply must not clear ae_inflight */
    m.prev_index = end;
    rc = log_term(r, end, &pt);
    if (rc != EFS_OK && end != 0)
        return EFS_OK;
    m.prev_term = pt;
    m.leader_commit = r->commit_index;
    rc = send_msg(r, &m);
    if (rc == EFS_OK)
        r->ae_inflight_commit[to] = r->commit_index;
    return EFS_OK;
}

/* Peer is at or behind the snapshot, or a get raced a snapshot and the
 * index is gone. Do not return NOT_FOUND: efs_raft_propose already
 * appended, and that code fails the caller's metadata RPC. A client
 * fsync then returns EIO ("report could not find the inode") and does
 * not retry. Heartbeat / the empty AppendEntries below retries. */
static int send_ae_behind(struct efs_raft *r, int to, int data_only,
                          uint64_t ni)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0;
    int rc;

    if (ae_inflight_fresh(r, to, ni))
        return EFS_OK;
    rc = send_snap(r, to);
    /* snap_idx 0: send_snap sent nothing. Still probe, or this peer
     * gets no AppendEntries and its election timer fires. */
    if (rc == EFS_OK && r->snap_idx == 0)
        rc = EFS_ERR_AGAIN;
    if (rc == EFS_ERR_AGAIN || rc == EFS_ERR_NOT_FOUND) {
        if (data_only)
            return EFS_OK;
        memset(&m, 0, sizeof(m));
        m.type = EFS_RAFT_MSG_AE_REQ;
        m.to = to;
        rc = last_log(r, &last_i, &last_t);
        if (rc != EFS_OK)
            return rc;
        m.prev_index = last_i;
        m.prev_term = last_t;
        m.leader_commit = r->commit_index;
        rc = send_msg(r, &m);
        if (rc == EFS_OK)
            ae_inflight_set(r, to, ni, last_i);
        if (rc == EFS_ERR_AGAIN)
            return EFS_OK;
        return rc;
    }
    return rc;
}

/* data_only: push entries that are already covered by send_idx. Do not
 * emit an empty heartbeat and do not clear ae_inflight when there is
 * nothing new — a heartbeat does both, and doing that on every propose
 * wake would drop the one-batch cap. */
static int send_ae(struct efs_raft *r, int to, int data_only)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0, prev_t = 0;
    uint64_t ni;
    int rc;

    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_AE_REQ;
    m.to = to;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    ni = r->next_index[to];
    if (ni == 0)
        ni = 1;
    if (ni <= r->snap_idx) {
        /* Same one-outstanding rule as the entry batch below: a snapshot
         * per propose to a peer in the snap window is a multi-MiB blob per
         * propose. on_snap_rep clears it; the heartbeat retransmits.
         * The snap file may not be ready yet. Sending nothing lets this
         * peer's election timer fire; its vote request carries a higher
         * term and steps the leader down for the whole export. Group 2
         * did that under 9-host posix (term +200, commit stuck, mkdir
         * EBUSY). send_ae_behind sends an empty AppendEntries in that
         * window. data_only must not: that marks ae_inflight and
         * suppresses the real snapshot for a heartbeat interval on
         * every propose wake. */
        return send_ae_behind(r, to, data_only, ni);
    }
    m.prev_index = ni - 1;
    rc = log_term(r, m.prev_index, &prev_t);
    if (rc == EFS_ERR_NOT_FOUND)
        return send_ae_behind(r, to, data_only, ni);
    if (rc != EFS_OK && m.prev_index != 0)
        return rc;
    m.prev_term = prev_t;
    m.leader_commit = r->commit_index;
    {
        /* Unsynced tail (appended, fsync still in the caller's hands)
         * stays out of this batch. A caught-up peer still gets the
         * empty heartbeat below. */
        uint64_t end = last_i;

        if (r->ae_capped && r->send_idx < end)
            end = r->send_idx;
        if (data_only && ni > end)
            return EFS_OK;
        if (ni <= end) {
        /* Batch the catch-up: read up to EFS_RAFT_AE_MAX entries (byte-
         * capped at EFS_RAFT_AE_BYTES) into one arena so a behind follower
         * recovers in one round-trip instead of one-entry-per-AE. Only
         * reached when this follower is behind (ni <= last_i); a caught-up
         * follower gets a bare heartbeat (nentries=0) and no arena. */
        uint32_t off = 0;

        /* One outstanding batch per behind peer. A reply (on_ae_rep) clears
         * ae_inflight and sends the next window; an unanswered batch is
         * resent once it is a heartbeat interval old. A commit that landed
         * after the batch was built is pushed as an empty probe so the
         * follower can apply without waiting out the heartbeat. */
        if (ae_inflight_fresh(r, to, ni)) {
            if (!data_only && r->commit_index > r->ae_inflight_commit[to])
                return send_commit_probe(r, to);
            return EFS_OK;
        }
        /* W19: the log read lands in the wire buffer. host_send writes
         * the 80-byte header and queues this pointer. No second copy. */
        {
            uint32_t cap = EFS_WIRE_RAFT_HDR_LEN + EFS_RAFT_AE_BYTES;
            uint8_t *frame = malloc(cap);

            if (!frame)
                return EFS_ERR_NOMEM;
            off = EFS_WIRE_RAFT_HDR_LEN;
            while (m.nentries < EFS_RAFT_AE_MAX && ni + m.nentries <= end &&
                   off + 12 < cap) {
                uint64_t eterm = 0;
                uint32_t ec = cap - off - 12;
                rc = r->store->get(r->store_ctx, ni + m.nentries, &eterm,
                                   frame + off + 12, &ec);
                if (rc == EFS_ERR_INVAL) {
                    if (m.nentries == 0) {
                        uint32_t need = EFS_WIRE_RAFT_HDR_LEN + 12u + ec;
                        uint8_t *big = malloc(need);
                        if (!big) {
                            free(frame);
                            return EFS_ERR_NOMEM;
                        }
                        rc = r->store->get(r->store_ctx, ni, &eterm,
                                           big + EFS_WIRE_RAFT_HDR_LEN + 12,
                                           &ec);
                        if (rc == EFS_ERR_NOT_FOUND) {
                            free(big);
                            free(frame);
                            return send_ae_behind(r, to, data_only, ni);
                        }
                        if (rc != EFS_OK) {
                            free(big);
                            free(frame);
                            return rc;
                        }
                        free(frame);
                        frame = big;
                        wr64(frame + EFS_WIRE_RAFT_HDR_LEN, eterm);
                        wr32(frame + EFS_WIRE_RAFT_HDR_LEN + 8, ec);
                        m.entries[0].term = eterm;
                        m.entries[0].clen = ec;
                        m.entries[0].cmd = big + EFS_WIRE_RAFT_HDR_LEN + 12;
                        m.nentries = 1;
                        off = need;
                    }
                    break;
                }
                if (rc == EFS_ERR_NOT_FOUND) {
                    free(frame);
                    return send_ae_behind(r, to, data_only, ni);
                }
                if (rc != EFS_OK) {
                    free(frame);
                    return rc;
                }
                wr64(frame + off, eterm);
                wr32(frame + off + 8, ec);
                m.entries[m.nentries].term = eterm;
                m.entries[m.nentries].clen = ec;
                m.entries[m.nentries].cmd = frame + off + 12;
                off += 12u + ec;
                m.nentries++;
            }
            if (m.nentries == 0) {
                free(frame);
                return EFS_ERR_PROTO;
            }
            m.wire = frame;
            m.wire_len = off;
        }
        }
    }
    rc = send_msg(r, &m);
    if (rc == EFS_OK)
        ae_inflight_set(r, to, m.nentries ? ni : 0,
                        m.nentries ? ni + m.nentries - 1 : m.prev_index);
    free(m.wire);
    /* The entry is already in the leader log. A full outbox must not
     * fail the proposer's RPC, and must not mark a batch in flight
     * that the sender will never write. */
    if (rc == EFS_ERR_AGAIN)
        return EFS_OK;
    return rc;
}

static int broadcast_ae(struct efs_raft *r)
{
    int i, rc = EFS_OK;
    uint32_t mask = peer_mask(r);

    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (i == r->id || !(mask & (1u << i)))
            continue;
        rc = send_ae(r, i, 0);
        /* One peer's compacted index must not skip the other peers'
         * heartbeats or fail a propose that already appended. */
        if (rc == EFS_ERR_NOT_FOUND)
            continue;
        if (rc != EFS_OK)
            return rc;
    }
    r->hb_elapsed = 0;
    return EFS_OK;
}

static int maybe_append_cold(struct efs_raft *r)
{
    uint8_t cmd[5];
    int rc;

    if (r->role != EFS_RAFT_LEADER || !r->log_new)
        return EFS_OK;
    if (r->commit_index < r->joint_idx)
        return EFS_OK;
    if (r->cold_idx > r->joint_idx)
        return EFS_OK;
    cmd[0] = EFS_RAFT_CMD_COLD;
    wr32(cmd + 1, r->log_new);
    rc = append_local(r, r->current_term, cmd, 5, NULL);
    if (rc != EFS_OK)
        return rc;
    return broadcast_ae(r);
}

/* Highest index both configs can already have a quorum for. Walking
 * from last_log re-reads every index a lagging match cannot cover. */
static uint64_t commit_ceiling(const struct efs_raft *r, uint64_t last_i)
{
    uint32_t sets[2];
    int ns = 0, s;
    uint64_t start = last_i;

    sets[ns++] = r->log_old;
    if (r->log_new)
        sets[ns++] = r->log_new;
    for (s = 0; s < ns; s++) {
        uint64_t m[EFS_RAFT_MAX_PEERS];
        int n = 0, i, need, a, b;

        need = popc(sets[s]) / 2 + 1;
        for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
            if ((sets[s] & (1u << i)) == 0)
                continue;
            if (r->ae_capped && i == r->id &&
                r->match_index[i] > r->durable_idx)
                m[n++] = r->durable_idx;
            else
                m[n++] = r->match_index[i];
        }
        if (n < need)
            return r->commit_index;
        for (a = 1; a < n; a++) {
            uint64_t v = m[a];
            b = a;
            while (b > 0 && m[b - 1] < v) {
                m[b] = m[b - 1];
                b--;
            }
            m[b] = v;
        }
        if (m[need - 1] < start)
            start = m[need - 1];
    }
    return start;
}

static int try_commit(struct efs_raft *r)
{
    uint64_t last_i = 0, last_t = 0, n, start;
    int rc;

    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    start = commit_ceiling(r, last_i);
    for (n = start; n > r->commit_index; n--) {
        uint64_t t = 0;
        unsigned bits = 0;
        int i;
        if (log_term(r, n, &t) != EFS_OK)
            break;
        if (t != r->current_term)
            break;
        for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
            /* Own match is set at append, before the fsync. Do not let
             * that vote commit an entry this process could still lose. */
            if (r->ae_capped && i == r->id && n > r->durable_idx)
                continue;
            if (r->match_index[i] >= n)
                bits |= 1u << i;
        }
        if (quorum_ok(r, bits)) {
            r->commit_index = n;
            /* Commit quorum in this term is a ReadIndex majority for n.
             * Without this, every propose invalidates read_current and the
             * next LOOKUP/CREATE pays another heartbeat RTT (~50 ms). */
            r->read_index = n;
            r->read_acks = bits;
            if (!r->read_in_flight)
                r->read_in_flight = 1;
            break;
        }
    }
    rc = apply_committed(r);
    if (rc != EFS_OK)
        return rc;
    return maybe_append_cold(r);
}

static int become_leader(struct efs_raft *r)
{
    int i;
    uint64_t last_i = 0, last_t = 0;
    uint32_t mask;
    int rc;

    r->role = EFS_RAFT_LEADER;
    r->leader = r->id;
    r->read_in_flight = 0;
    r->read_index = 0;
    r->read_acks = 0;
    reset_election(r);
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    mask = peer_mask(r) | (1u << r->id);
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (!(mask & (1u << i)))
            continue;
        r->next_index[i] = last_i + 1;
        r->match_index[i] = (i == r->id) ? last_i : 0;
    }
    memset(r->ae_inflight, 0, sizeof(r->ae_inflight));
    memset(r->ae_inflight_commit, 0, sizeof(r->ae_inflight_commit));
    memset(r->ae_inflight_end, 0, sizeof(r->ae_inflight_end));
    rc = append_local(r, r->current_term, NULL, 0, NULL);
    if (rc != EFS_OK)
        return rc;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    r->match_index[r->id] = last_i;
    if (solo(r)) {
        r->commit_index = last_i;
        apply_committed(r);
        return maybe_append_cold(r);
    }
    rc = maybe_append_cold(r);
    if (rc != EFS_OK)
        return rc;
    return broadcast_ae(r);
}

static int start_election(struct efs_raft *r)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0;
    int i, rc;

    if (!is_voter(r, r->id) || !r->allow_campaign) {
        reset_election(r);
        return EFS_OK;
    }
    r->current_term++;
    r->voted_for = (int32_t)r->id;
    r->role = EFS_RAFT_CANDIDATE;
    r->leader = -1;
    r->vote_bits = 1u << r->id;
    reset_election(r);
    rc = save_hard(r);
    if (rc != EFS_OK)
        return rc;
    if (solo(r))
        return become_leader(r);
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_VOTE_REQ;
    m.last_log_index = last_i;
    m.last_log_term = last_t;
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (i == r->id || !is_voter(r, i))
            continue;
        m.to = i;
        rc = send_msg(r, &m);
        if (rc == EFS_ERR_AGAIN)
            continue;
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

static int on_vote_req(struct efs_raft *r, const struct efs_raft_msg *in)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0;
    int grant = 0;
    int rc;

    maybe_step_down(r, in->term);
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_VOTE_REP;
    m.to = in->from;
    if (in->term < r->current_term || !is_voter(r, r->id)) {
        m.vote_granted = 0;
        return send_msg(r, &m);
    }
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    if ((r->voted_for == -1 || r->voted_for == in->from) &&
        (in->last_log_term > last_t ||
         (in->last_log_term == last_t && in->last_log_index >= last_i))) {
        grant = 1;
        r->voted_for = in->from;
        reset_election(r);
        rc = save_hard(r);
        if (rc != EFS_OK)
            return rc;
    }
    m.vote_granted = grant;
    return send_msg(r, &m);
}

static int on_vote_rep(struct efs_raft *r, const struct efs_raft_msg *in)
{
    maybe_step_down(r, in->term);
    if (r->role != EFS_RAFT_CANDIDATE || in->term != r->current_term)
        return EFS_OK;
    if (in->vote_granted) {
        if (in->from >= 0 && in->from < EFS_RAFT_MAX_PEERS)
            r->vote_bits |= 1u << in->from;
        if (quorum_ok(r, r->vote_bits))
            return become_leader(r);
    }
    return EFS_OK;
}

static int on_ae_req(struct efs_raft *r, const struct efs_raft_msg *in)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0, pt = 0;
    int rc;

    maybe_step_down(r, in->term);
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_AE_REP;
    m.to = in->from;
    m.vote_granted = in->vote_granted; /* echo a commit probe */
    if (in->term < r->current_term) {
        m.success = 0;
        return send_msg(r, &m);
    }
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = in->from;
    reset_election(r);
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    {
        static int ae_dbg[EFS_RAFT_MAX_PEERS];
        int *ctr = &ae_dbg[in->from >= 0 && in->from < EFS_RAFT_MAX_PEERS ? in->from : 0];
        (*ctr)++;
        if (getenv("EFS_RAFT_AE_DBG") && (*ctr <= 5 || *ctr % 1000 == 0))
            fprintf(stderr, "AE_REQ from=%d prev_idx=%llu prev_term=%llu nent=%u my_last=%llu my_term=%llu in_term=%llu\n",
                    in->from, (unsigned long long)in->prev_index,
                    (unsigned long long)in->prev_term, in->nentries,
                    (unsigned long long)last_i, (unsigned long long)r->current_term,
                    (unsigned long long)in->term);
    }
    if (in->prev_index > last_i) {
        if (getenv("EFS_RAFT_AE_DBG"))
            fprintf(stderr, "AE_REQ REJECT prev_idx=%llu > last_i=%llu\n",
                    (unsigned long long)in->prev_index, (unsigned long long)last_i);
        m.success = 0;
        m.match_index = last_i;
        return send_msg(r, &m);
    }
    rc = log_term(r, in->prev_index, &pt);
    if ((in->prev_index > 0 && rc != EFS_OK) || pt != in->prev_term) {
        if (getenv("EFS_RAFT_AE_DBG"))
            fprintf(stderr, "AE_REQ REJECT prev_term: pt=%llu in_prev_term=%llu rc=%d prev_idx=%llu\n",
                    (unsigned long long)pt, (unsigned long long)in->prev_term, rc,
                    (unsigned long long)in->prev_index);
        if (in->prev_index > r->snap_idx) {
            r->store->truncate_from(r->store_ctx, in->prev_index);
            reload_cfg_from_log(r);
        }
        m.success = 0;
        last_log(r, &last_i, &last_t);
        m.match_index = last_i;
        return send_msg(r, &m);
    }
    if (in->nentries) {
        uint64_t idx = in->prev_index + 1;
        uint32_t i;
        int batch = in->nentries > 1 && r->store->batch_begin &&
                    r->store->batch_end;

        if (batch && r->store->batch_begin(r->store_ctx) != EFS_OK)
            batch = 0;
        for (i = 0; i < in->nentries; i++, idx++) {
            uint64_t et = 0;
            rc = log_term(r, idx, &et);
            if (rc == EFS_OK && et == in->entries[i].term)
                /* Already have this entry (duplicate / overlapping batch):
                 * same index + same term ⇒ same command (Log Matching).
                 * Rewriting it costs a pwrite + fsync per entry — a
                 * duplicated 128-entry batch was ~30 ms of pump time under
                 * h->mu, which is how a behind follower fell over. */
                continue;
            if (rc == EFS_OK) {
                /* Conflict at idx: drop it and everything after, then take
                 * the leader's entry. (Same rule as the single-entry path,
                 * applied per batched entry.) */
                r->store->truncate_from(r->store_ctx, idx);
                reload_cfg_from_log(r);
            }
            rc = r->store->append(r->store_ctx, idx, in->entries[i].term,
                                  in->entries[i].cmd, in->entries[i].clen);
            if (rc != EFS_OK) {
                if (getenv("EFS_RAFT_AE_DBG"))
                    fprintf(stderr, "AE_REQ APPEND FAIL idx=%llu i=%u rc=%d\n",
                            (unsigned long long)idx, i, rc);
                if (batch)
                    (void)r->store->batch_end(r->store_ctx);
                return rc;
            }
            install_log_cfg(r, in->entries[i].cmd, in->entries[i].clen, idx);
        }
        if (batch) {
            rc = r->store->batch_end(r->store_ctx);
            if (rc != EFS_OK)
                return rc;
        }
        last_log(r, &last_i, &last_t);
        if (getenv("EFS_RAFT_AE_DBG"))
            fprintf(stderr, "AE_REQ ACCEPT nent=%u new_last=%llu\n",
                    in->nentries, (unsigned long long)last_i);
    }
    if (in->leader_commit > r->commit_index) {
        uint64_t cap = last_i;
        r->commit_index = in->leader_commit < cap ? in->leader_commit : cap;
        apply_committed(r);
    }
    last_log(r, &last_i, &last_t);
    m.success = 1;
    m.match_index = last_i;
    return send_msg(r, &m);
}

static int on_ae_rep(struct efs_raft *r, const struct efs_raft_msg *in)
{
    maybe_step_down(r, in->term);
    if (r->role != EFS_RAFT_LEADER || in->term != r->current_term)
        return EFS_OK;
    if (in->from < 0 || in->from >= EFS_RAFT_MAX_PEERS)
        return EFS_OK;
    /* A commit probe echoes vote_granted. Clearing inflight here would
     * forget an entry batch that is still unanswered. */
    if (!in->vote_granted)
        r->ae_inflight[in->from] = 0;
    if (in->success) {
        uint64_t prev_commit = r->commit_index;
        if (in->match_index > r->match_index[in->from])
            r->match_index[in->from] = in->match_index;
        r->next_index[in->from] = r->match_index[in->from] + 1;
        if (r->read_in_flight == 1)
            r->read_acks |= 1u << in->from;
        /* Queue the next batch before apply. The sender thread writes
         * it while apply still holds the lock, so the round trip
         * overlaps the apply. send_ae will not start a second batch
         * while this one is in flight. */
        if (r->next_index[in->from] <= r->match_index[r->id])
            (void)send_ae(r, in->from, 1);
        try_commit(r);
        apply_committed(r);
        if (r->commit_index != prev_commit) {
            /* Commit advanced: push the new commit index to every peer NOW.
             * A follower's local apply (and anyone waiting on it, e.g. a
             * client op that landed on that follower) would otherwise stall
             * until the next scheduled heartbeat. */
            broadcast_ae(r);
            return EFS_OK;
        }
        return EFS_OK;
    }
    /* Rejection: the follower's reply carries match_index = its last log
     * index (after any conflict truncation). Jump straight there instead of
     * decrementing one index per round trip — a fresh/rejoined follower
     * catching up a long log would otherwise need one RTT per index, and at
     * the heartbeat-gated rate that is minutes of the follower being
     * unusable for quorum reads. match_index is 0 for a term rejection,
     * which safely restarts from index 1.
     * A commit probe sets vote_granted so its reply does not clear
     * ae_inflight on the success path. A rejection must still clear it:
     * the follower does not have the outstanding batch, and leaving
     * inflight set held the resend until the next heartbeat (apply sat
     * one index behind for 50 ms; the wire ACK was ~10 us). */
    r->ae_inflight[in->from] = 0;
    {
        uint64_t ni = in->match_index + 1, last_i = 0, last_t = 0;
        last_log(r, &last_i, &last_t);
        if (ni > last_i + 1)
            ni = last_i + 1;
        if (ni < 1)
            ni = 1;
        r->next_index[in->from] = ni;
    }
    return send_ae(r, in->from, 0);
}

static int snap_reject(struct efs_raft *r, struct efs_raft_msg *m,
                       uint64_t last_i)
{
    r->rx_off = 0;
    m->success = 0;
    m->vote_granted = 0;
    m->match_index = last_i;
    return send_msg(r, m);
}

static int snap_installed(struct efs_raft *r, struct efs_raft_msg *m,
                          uint64_t incl, uint64_t incl_t, uint32_t cfg_old,
                          uint32_t cfg_new, int take_cfg)
{
    uint64_t last_i = 0, last_t = 0;
    int rc;

    rc = r->store->save_snap(r->store_ctx, incl, incl_t);
    if (rc != EFS_OK)
        return rc;
    if (take_cfg) {
        r->last_applied = incl;
        r->app_old = r->log_old = cfg_old;
        r->app_new = r->log_new = cfg_new;
    }
    r->snap_idx = incl;
    r->snap_term = incl_t;
    r->rx_off = 0;
    if (r->commit_index < r->last_applied)
        r->commit_index = r->last_applied;
    if (m->leader_commit > r->commit_index) {
        last_log(r, &last_i, &last_t);
        r->commit_index =
            m->leader_commit < last_i ? m->leader_commit : last_i;
        if (r->commit_index < r->last_applied)
            r->commit_index = r->last_applied;
        apply_committed(r);
    }
    reload_cfg_from_log(r);
    save_cfg(r);
    if (getenv("EFS_RAFT_DBG"))
        fprintf(stderr, "raft[%u]: on_snap_req INSTALLED incl=%llu\n",
                r->group, (unsigned long long)incl);
    m->success = 1;
    m->vote_granted = 1;
    m->match_index = incl;
    m->prev_index = r->rx_off;
    return send_msg(r, m);
}

static int on_snap_req(struct efs_raft *r, const struct efs_raft_msg *in)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0, incl, incl_t, offset;
    const uint8_t *pay = NULL, *user = NULL;
    uint32_t plen = 0, ulen = 0;
    int done, rc;

    maybe_step_down(r, in->term);
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_SNAP_REP;
    m.to = in->from;
    m.leader_commit = in->leader_commit;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    if (in->term < r->current_term) {
        m.success = 0;
        m.match_index = last_i;
        return send_msg(r, &m);
    }
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = in->from;
    reset_election(r);
    incl = in->last_log_index;
    incl_t = in->last_log_term;
    offset = in->prev_index;
    done = in->success ? 1 : 0;
    if (in->nentries >= 1) {
        pay = in->entries[0].cmd;
        plen = in->entries[0].clen;
    }
    if (getenv("EFS_RAFT_DBG"))
        fprintf(stderr, "raft[%u]: on_snap_req from=%d incl=%llu off=%llu "
                "done=%d plen=%u my_applied=%llu my_snap=%llu\n",
                r->group, in->from, (unsigned long long)incl,
                (unsigned long long)offset, done, plen,
                (unsigned long long)r->last_applied,
                (unsigned long long)r->snap_idx);
    if (incl == 0 || incl < r->snap_idx ||
        (incl <= r->last_applied && incl <= r->snap_idx)) {
        m.success = 1;
        m.vote_granted = 1;
        m.match_index = r->snap_idx ? r->snap_idx : last_i;
        return send_msg(r, &m);
    }
    /* Already applied through incl: truncate, do not import an older
     * image over a newer KV. */
    if (r->last_applied >= incl) {
        return snap_installed(r, &m, incl, incl_t, 0, 0, 0);
    }
    if (offset == 0) {
        if (plen < 8 || !pay || !r->snap_chunk)
            return snap_reject(r, &m, last_i);
        r->rx_incl = incl;
        r->rx_term = incl_t;
        r->rx_off = 0;
        r->rx_old = rd32(pay);
        r->rx_new = rd32(pay + 4);
        user = pay + 8;
        ulen = plen - 8;
    } else {
        if (incl != r->rx_incl || incl_t != r->rx_term)
            return snap_reject(r, &m, last_i);
        user = pay;
        ulen = plen;
        if (offset + ulen == r->rx_off) {
            m.success = 1;
            m.prev_index = r->rx_off;
            return send_msg(r, &m);
        }
        if (offset != r->rx_off)
            return snap_reject(r, &m, last_i);
    }
    rc = r->snap_chunk(r->app, incl, incl_t, offset, user, ulen, done);
    if (rc == EFS_ERR_BUSY) {
        /* Chunk is on disk; the KV diff is still running off this thread.
         * Ack this same offset so the leader retries it. snap_reject would
         * clear rx_off and the next chunk would restart from byte 0. */
        m.success = 1;
        m.vote_granted = 0;
        m.prev_index = offset;
        return send_msg(r, &m);
    }
    if (rc != EFS_OK) {
        if (getenv("EFS_RAFT_DBG"))
            fprintf(stderr, "raft[%u]: on_snap_req chunk rc=%d incl=%llu "
                    "off=%llu\n", r->group, rc, (unsigned long long)incl,
                    (unsigned long long)offset);
        return snap_reject(r, &m, last_i);
    }
    r->rx_off = offset + ulen;
    if (!done) {
        m.success = 1;
        m.vote_granted = 0;
        m.prev_index = r->rx_off;
        return send_msg(r, &m);
    }
    return snap_installed(r, &m, incl, incl_t, r->rx_old, r->rx_new, 1);
}

static int on_snap_rep(struct efs_raft *r, const struct efs_raft_msg *in)
{
    int rc;

    maybe_step_down(r, in->term);
    if (r->role != EFS_RAFT_LEADER || in->term != r->current_term)
        return EFS_OK;
    if (in->from < 0 || in->from >= EFS_RAFT_MAX_PEERS)
        return EFS_OK;
    r->ae_inflight[in->from] = 0;
    if (!in->success) {
        if (getenv("EFS_RAFT_DBG"))
            fprintf(stderr, "raft[%u]: on_snap_rep REJECT from=%d match=%llu "
                    "snap_idx=%llu\n", r->group, in->from,
                    (unsigned long long)in->match_index,
                    (unsigned long long)r->snap_idx);
        r->snap_off[in->from] = 0;
        r->snap_retry_tick[in->from] = 0;
        if (r->snap_idx)
            r->next_index[in->from] = r->snap_idx;
        return EFS_OK;
    }
    if (!in->vote_granted) {
        /* Same offset: the follower is still importing. One chunk is
         * already in flight; the next heartbeat retries. Propose wakes
         * must not pread the chunk again. */
        if (in->prev_index == r->snap_off[in->from]) {
            uint32_t hb = r->heartbeat_ticks ? r->heartbeat_ticks : 1;

            r->snap_retry_tick[in->from] = r->ticks + hb;
            return EFS_OK;
        }
        r->snap_retry_tick[in->from] = 0;
        r->snap_off[in->from] = in->prev_index;
        rc = send_snap(r, in->from);
        if (rc == EFS_ERR_AGAIN)
            return EFS_OK;
        return rc;
    }
    if (in->match_index > r->match_index[in->from])
        r->match_index[in->from] = in->match_index;
    r->next_index[in->from] = r->match_index[in->from] + 1;
    r->snap_off[in->from] = 0;
    r->snap_retry_tick[in->from] = 0;
    try_commit(r);
    apply_committed(r);
    if (r->next_index[in->from] <= r->match_index[r->id])
        return send_ae(r, in->from, 0);
    return EFS_OK;
}

static int valid_voters(uint32_t v)
{
    int n, i;

    if (v == 0 || (v >> EFS_RAFT_MAX_PEERS))
        return 0;
    n = popc(v);
    if (n < 1 || (n % 2) == 0)
        return 0;
    for (i = EFS_RAFT_MAX_PEERS; i < 32; i++) {
        if (v & (1u << i))
            return 0;
    }
    return 1;
}

struct efs_raft *efs_raft_new(const struct efs_raft_cfg *cfg)
{
    struct efs_raft *r;
    uint32_t voters;
    int rc;

    if (!cfg || !cfg->store || cfg->n < 1 || cfg->n > EFS_RAFT_MAX_PEERS)
        return NULL;
    if (cfg->id < 0 || cfg->id >= EFS_RAFT_MAX_PEERS)
        return NULL;
    if (!cfg->store->save_hard || !cfg->send)
        return NULL;
    voters = cfg->voters ? cfg->voters : ((1u << cfg->n) - 1);
    if (!valid_voters(voters))
        return NULL;
    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->id = cfg->id;
    r->group = cfg->group;
    r->boot_id = cfg->boot_id ? cfg->boot_id : 1;
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = -1;
    r->voted_for = -1;
    r->log_old = r->app_old = voters;
    r->election_ticks = cfg->election_ticks ? cfg->election_ticks : 10;
    r->heartbeat_ticks = cfg->heartbeat_ticks ? cfg->heartbeat_ticks : 1;
    r->rng = cfg->rng_seed;
    if (!r->rng)
        r->rng = 0x9e3779b97f4a7c15ULL ^
                 ((uint64_t)(r->id + 1) * 0x100000001b3ULL) ^
                 r->boot_id ^ ((uint64_t)r->group << 48);
    reset_election(r);
    r->store = cfg->store;
    r->store_ctx = cfg->store_ctx ? cfg->store_ctx : cfg->store;
    r->send = cfg->send;
    r->net = cfg->net;
    r->apply = cfg->apply;
    r->app = cfg->app;
    r->snap_open = cfg->snap_open;
    r->snap_read = cfg->snap_read;
    r->snap_close = cfg->snap_close;
    r->snap_chunk = cfg->snap_chunk;
    r->snap_chunk_bytes = cfg->snap_chunk_bytes;
    rc = r->store->load_hard(r->store_ctx, &r->current_term, &r->voted_for);
    if (rc != EFS_OK) {
        free(r);
        return NULL;
    }
    if (r->store->load_snap)
        r->store->load_snap(r->store_ctx, &r->snap_idx, &r->snap_term);
    if (r->store->load_cfg) {
        uint32_t o = 0, n = 0;
        rc = r->store->load_cfg(r->store_ctx, &o, &n);
        if (rc == EFS_OK) {
            r->app_old = r->log_old = o;
            r->app_new = r->log_new = n;
        }
    }
    r->allow_campaign = 1;
    r->last_applied = r->snap_idx;
    r->commit_index = r->snap_idx;
    if (reload_cfg_from_log(r) != EFS_OK) {
        free(r);
        return NULL;
    }
    return r;
}

void efs_raft_free(struct efs_raft *r)
{
    if (!r)
        return;
    if (r->snap_handle && r->snap_close)
        r->snap_close(r->snap_handle);
    free(r);
}

int efs_raft_tick(struct efs_raft *r)
{
    if (!r)
        return EFS_ERR_INVAL;
    r->ticks++;
    if (r->role == EFS_RAFT_LEADER) {
        r->hb_elapsed++;
        if (r->hb_elapsed >= r->heartbeat_ticks)
            return broadcast_ae(r);
        return EFS_OK;
    }
    r->election_elapsed++;
    if (r->election_elapsed >= r->election_deadline)
        return start_election(r);
    return EFS_OK;
}

int efs_raft_recv(struct efs_raft *r, const struct efs_raft_msg *msg)
{
    uint64_t boot;

    if (!r || !msg)
        return EFS_ERR_INVAL;
    if (msg->to != r->id)
        return EFS_ERR_INVAL;
    if (msg->from < 0 || msg->from >= EFS_RAFT_MAX_PEERS)
        return EFS_ERR_INVAL;
    boot = msg->boot_id ? msg->boot_id : 1;
    if (r->peer_boot[msg->from] && boot < r->peer_boot[msg->from])
        return EFS_OK;
    if (boot > r->peer_boot[msg->from])
        r->peer_boot[msg->from] = boot;
    switch (msg->type) {
    case EFS_RAFT_MSG_VOTE_REQ:
        return on_vote_req(r, msg);
    case EFS_RAFT_MSG_VOTE_REP:
        return on_vote_rep(r, msg);
    case EFS_RAFT_MSG_AE_REQ:
        return on_ae_req(r, msg);
    case EFS_RAFT_MSG_AE_REP:
        return on_ae_rep(r, msg);
    case EFS_RAFT_MSG_SNAP_REQ:
        return on_snap_req(r, msg);
    case EFS_RAFT_MSG_SNAP_REP:
        return on_snap_rep(r, msg);
    default:
        return EFS_ERR_INVAL;
    }
}

int efs_raft_propose(struct efs_raft *r, const uint8_t *cmd, uint32_t clen,
                     uint64_t *index_out)
{
    int rc;
    uint64_t ix = 0;

    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    if (is_cfg_cmd(cmd, clen))
        return EFS_ERR_INVAL;
    rc = append_local(r, r->current_term, cmd, clen, &ix);
    if (rc != EFS_OK)
        return rc;
    if (index_out)
        *index_out = ix;
    if (solo(r)) {
        uint64_t last_i = 0, last_t = 0;
        last_log(r, &last_i, &last_t);
        r->commit_index = last_i;
        rc = apply_committed(r);
        if (rc != EFS_OK)
            return rc;
        return maybe_append_cold(r);
    }
    if (r->ae_capped && ix > r->send_idx)
        r->send_idx = ix;
    return broadcast_ae(r);
}

/* Arm before the first quiet append so a heartbeat cannot carry an
 * entry whose fsync has not finished. The replayed log is durable. */
void efs_raft_arm_durable(struct efs_raft *r)
{
    uint64_t last_i = 0, last_t = 0;

    if (!r || r->ae_capped)
        return;
    if (last_log(r, &last_i, &last_t) != EFS_OK)
        return;
    r->send_idx = last_i;
    r->durable_idx = last_i;
    r->ae_capped = 1;
}

int efs_raft_propose_local(struct efs_raft *r, const uint8_t *cmd,
                           uint32_t clen, uint64_t *index_out)
{
    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    if (is_cfg_cmd(cmd, clen))
        return EFS_ERR_INVAL;
    if (solo(r))
        return EFS_ERR_INVAL;
    efs_raft_arm_durable(r);
    return append_local(r, r->current_term, cmd, clen, index_out);
}

int efs_raft_submit(struct efs_raft *r, uint64_t idx)
{
    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    efs_raft_arm_durable(r);
    if (idx > r->send_idx)
        r->send_idx = idx;
    /* Do not transmit here. The caller still holds the state-machine
     * lock, so a send now would be this one entry; the pump flushes
     * every index covered by send_idx in one batch. */
    return EFS_OK;
}

int efs_raft_flush(struct efs_raft *r)
{
    int i, rc = EFS_OK;
    uint32_t mask;

    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    mask = peer_mask(r);
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (i == r->id || !(mask & (1u << i)))
            continue;
        rc = send_ae(r, i, 1);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

int efs_raft_durable(struct efs_raft *r, uint64_t idx)
{
    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    efs_raft_arm_durable(r);
    if (idx > r->durable_idx)
        r->durable_idx = idx;
    return try_commit(r);
}

int efs_raft_change(struct efs_raft *r, uint32_t new_voters)
{
    uint32_t add;
    uint8_t cmd[9];
    uint64_t last_i = 0, last_t = 0;
    int i, rc;

    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    if (!valid_voters(new_voters))
        return EFS_ERR_INVAL;
    if (r->log_new)
        return EFS_ERR_BUSY;
    if (r->log_old == new_voters && r->app_old == new_voters)
        return EFS_OK;
    add = new_voters & ~r->log_old;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (!(add & (1u << i)))
            continue;
        r->learners |= 1u << i;
        if (r->next_index[i] == 0)
            r->next_index[i] = last_i + 1;
    }
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (!(add & (1u << i)))
            continue;
        if (r->match_index[i] < r->commit_index) {
            broadcast_ae(r);
            return EFS_ERR_BUSY;
        }
    }
    cmd[0] = EFS_RAFT_CMD_JOINT;
    wr32(cmd + 1, r->log_old);
    wr32(cmd + 5, new_voters);
    rc = append_local(r, r->current_term, cmd, 9, NULL);
    if (rc != EFS_OK)
        return rc;
    if (solo(r)) {
        last_log(r, &last_i, &last_t);
        r->commit_index = last_i;
        apply_committed(r);
        return maybe_append_cold(r);
    }
    rc = broadcast_ae(r);
    if (rc != EFS_OK)
        return rc;
    return try_commit(r);
}

int efs_raft_restore_applied(struct efs_raft *r, uint64_t idx)
{
    uint64_t last_i = 0, last_t = 0;
    int rc;

    if (!r)
        return EFS_ERR_INVAL;
    if (idx == 0)
        return EFS_OK;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    if (idx > last_i)
        idx = last_i;
    if (idx < r->snap_idx)
        idx = r->snap_idx;
    r->last_applied = idx;
    if (r->commit_index < idx)
        r->commit_index = idx;
    return EFS_OK;
}

int efs_raft_snapshot(struct efs_raft *r)
{
    uint64_t t = 0;
    int rc;

    if (!r || r->last_applied == 0)
        return EFS_OK;
    if (r->last_applied <= r->snap_idx)
        return EFS_OK;
    if (r->snap_open) {
        void *h = NULL;
        uint64_t total = 0;

        rc = r->snap_open(r->app, r->last_applied, &h, &total);
        if (rc == EFS_ERR_BUSY) {
            /* Export is queued. The snap point below is this index;
             * send_snap reads the file when it is ready. */
        } else if (rc != EFS_OK) {
            return rc;
        } else {
            if (r->snap_handle && r->snap_close)
                r->snap_close(r->snap_handle);
            r->snap_handle = h;
            r->snap_total = total;
            r->snap_handle_incl = r->last_applied;
        }
    }
    rc = log_term(r, r->last_applied, &t);
    if (rc != EFS_OK)
        return rc;
    rc = r->store->save_snap(r->store_ctx, r->last_applied, t);
    if (rc != EFS_OK)
        return rc;
    r->snap_idx = r->last_applied;
    r->snap_term = t;
    return save_cfg(r);
}

int efs_raft_role(const struct efs_raft *r)
{
    return r ? r->role : -1;
}

uint64_t efs_raft_term(const struct efs_raft *r)
{
    return r ? r->current_term : 0;
}

uint64_t efs_raft_commit(const struct efs_raft *r)
{
    return r ? r->commit_index : 0;
}

uint64_t efs_raft_applied(const struct efs_raft *r)
{
    return r ? r->last_applied : 0;
}

uint64_t efs_raft_snap_index(const struct efs_raft *r)
{
    return r ? r->snap_idx : 0;
}

int efs_raft_leader(const struct efs_raft *r)
{
    return r ? r->leader : -1;
}

uint32_t efs_raft_voters(const struct efs_raft *r)
{
    return r ? r->app_old : 0;
}

int efs_raft_joint(const struct efs_raft *r)
{
    return r && r->app_new != 0;
}

int efs_raft_step_down(struct efs_raft *r)
{
    if (!r)
        return EFS_ERR_INVAL;
    if (r->role == EFS_RAFT_FOLLOWER)
        return EFS_OK;
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = -1;
    r->vote_bits = 0;
    r->read_in_flight = 0;
    reset_election(r);
    return EFS_OK;
}

void efs_raft_allow_campaign(struct efs_raft *r, int on)
{
    if (r)
        r->allow_campaign = on ? 1 : 0;
}

int efs_raft_read_begin(struct efs_raft *r)
{
    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    r->read_index = r->commit_index;
    r->read_acks = 1u << r->id;
    r->read_in_flight = 1;
    if (solo(r)) {
        apply_committed(r);
        if (r->last_applied >= r->read_index && quorum_ok(r, r->read_acks))
            r->read_in_flight = 2;
        return EFS_OK;
    }
    return broadcast_ae(r);
}

int efs_raft_read_ready(const struct efs_raft *r)
{
    if (!r)
        return 0;
    return r->read_in_flight == 2 && r->last_applied >= r->read_index;
}

int efs_raft_read_current(const struct efs_raft *r)
{
    if (!r || r->role != EFS_RAFT_LEADER)
        return 0;
    return r->read_in_flight == 2 &&
           r->last_applied >= r->commit_index &&
           r->read_index >= r->commit_index;
}

int efs_raft_read_pending(const struct efs_raft *r)
{
    return r && r->role == EFS_RAFT_LEADER && r->read_in_flight == 1;
}

uint64_t efs_raft_read_index(const struct efs_raft *r)
{
    return r ? r->read_index : 0;
}

int efs_raft_read_done(const struct efs_raft *r)
{
    return r && r->role == EFS_RAFT_LEADER && r->read_in_flight == 2;
}

int efs_raft_read_covers(const struct efs_raft *r, uint64_t want)
{
    if (!r || r->role != EFS_RAFT_LEADER)
        return 0;
    return r->read_in_flight == 2 && r->read_index >= want &&
           r->last_applied >= r->read_index;
}
