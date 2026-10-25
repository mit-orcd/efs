#!/usr/bin/env python3
"""D31 revoked-stream drain (K13): dead and departing clients' tickets.

Production code on a durable LSM KV (reopened as a new leader finds it):
the leader's drain pass, the DRAIN apply and K32's ADMIT_BATCH /
SETTLE_BATCH apply (all from src/server/raft_host.c), the ticket ledger,
the session state machine, the shared revocation coordinator
(efs_session_revoke, as a clean unmount runs it) and the orphan collector.
Three storage members are modelled as in tests/test_put_orphan.c.

1. Dead client: tickets admitted by ADMIT_BATCH and single ADMIT, bodies
   stored, one published, one rejected, no SETTLE. Before revocation the
   drain refuses and nothing is collected. After it, late K32 batches are
   fenced, the drain moves every unsettled ticket to the orphan ledger and
   retires the published head, and the collector deletes every body except
   the published one. Both groups' anchors are drained.
2. A pending K14 stage makes the drain refuse at that ticket; once the
   stage resolves (ABORT) the drain completes and the body is reclaimed.
3. Clean unmount: settle what can be settled, then revoke the own session;
   the leaders drain and collect the rest: no ADMITTED ticket and no body
   left except the published ones.
4. Leader change during the drain of a long stream: a lost proposal, a lost
   reply, leadership lost mid-pass, a KV reopen, and the old leader's
   delayed DRAIN applied after the new leader finished (a no-op).
"""
from pathlib import Path
import os, shlex, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
if not (root / 'libefs.a').exists():
    sys.exit('build libefs.a first (make libefs.a)')
s = (root / 'src/server/raft_host.c').read_text()
def fn(head):
    a = s.index(head)
    while s.find(';', a) < s.find('{', a):
        a = s.index(head, a + 1)
    return s[a:s.index('\n}', a) + 2]
unpack = fn('static int host_unpack_ticket_batch(')
batch = fn('static int apply_put_ticket_batch_cmd(')
drain_apply = fn('static int apply_put_ticket_drain_cmd(')
scan_work = fn('static int host_gc_scan_work(')
a = s.index('/* ---- D31 revoked-stream drain (K13) ----')
pass_block = s[a:s.index('static void *host_gc_thread(void *arg)', a)]

# Wiring, by source shape: the leader drains before it collects, and a clean
# unmount settles, then revokes its own session through the shared
# coordinator, while its connections are still up.
gc = fn('static void *host_gc_thread(')
assert gc.index('host_put_drain_pass(h, h->g[g].group, anchor);') < gc.index('host_put_orphan_pass(h, h->g[g].group, anchor);')
assert 'h->put_drain_cursor_len[gc_gi(h->g[g].group)] = 0;' in gc, 'cursor reset on lost leadership'
nc = (root / 'src/client/node_cache.c').read_text()
sd = nc[nc.index('int efs_client_shutdown(void)'):]
order = [sd.index(x) for x in ('efs_client_stop_meta_flush();', 'settle_rc = efs_client_put_settle();',
                               'revoke_rc = efs_client_session_end();', 'unmount not clean',
                               'if (conn_pool_inited)')]
assert order == sorted(order), order
ir = (root / 'src/client/inode_rpc.c').read_text()
end = ir[ir.index('int efs_client_session_end(void)'):]
end = end[:end.index('\n}\n')]
assert 'efs_session_revoke(&rpc,id.client_uuid,&res)' in end and 'session_fenced,1' in end
# D42: always revoke (no keep-open switch); a failed flush, settle or revoke
# makes efs-fuse exit 1 instead of reporting a clean unmount.
assert 'getenv' not in end, 'clean unmount revoke has no switch'
assert 'return result;' in sd
fz = (root / 'src/client/efs_fuse.c').read_text()
assert 'if (efs_client_shutdown() != EFS_OK)\n        unmount_unclean(efs_client_shutdown_reason());' in fz
# A11 finding 1: the verdict reaches the stop record the stop tooling reads.
assert 'efs_stop_status("failed", 0, 0, ret, why);' in fz and 'efs_stop_status("done", 0, 0, 0, NULL);' in fz
assert 'if (!ret && g_unmount_unclean) {' in fz and 'ret = 1;' in fz
mg = (root / 'src/mgmt/efs_mgmt.c').read_text()
assert 'efs_session_revoke(&rpc, uuid, &res)' in mg, 'efs-mgmt fence uses the same coordinator'

code = r'''
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/protocol.h"
#include "efs/put_orphan.h"
#include "efs/raft.h"
#include "efs/session.h"
#include "efs/session_revoke.h"
#include "efs/store.h"
#include "efs/txn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define HOST_NGROUPS 2
#define MEMBERS 3
struct server_counters { uint64_t gc_scan_errors, put_orphan_failures; };
struct efs_raft_host {
    struct efs_kv *kv;
    int gc_running;
    uint8_t put_drain_cursor[HOST_NGROUPS][EFS_PUT_STREAM_KEY_LEN];
    uint32_t put_drain_cursor_len[HOST_NGROUPS];
    struct server_counters *s;
};
static struct server_counters counters;
static struct efs_raft_host host = {.gc_running = 1, .s = &counters};
static void wr32be(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (24 - 8 * i)); }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int gc_gi(uint8_t group) { return group == 0 ? 0 : 1; }
static int env_on(const char *name) { const char *v = getenv(name); return v && v[0] && strcmp(v, "0"); }

/* ---- the group leader's log, as the drain pass sees it ---- */
static unsigned proposals, commits, revoked_reads;
static int lose_before, lose_after, step_down_at; /* one-shot faults */
static uint8_t delayed[EFS_MD_PUT_TICKET_DRAIN_LEN];
static int keep_delayed;
static int put_orphan_revoked(void *ctx, const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch)
{
    (void)ctx;
    revoked_reads++;
    return efs_session_barrier_done(host.kv, uuid, epoch);
}
static int apply_put_ticket_drain_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                      uint32_t clen, uint8_t group);
static int host_gc_propose(struct efs_raft_host *h, uint8_t group, const uint8_t *cmd, uint32_t clen)
{
    assert(cmd[0] == EFS_MD_CMD_PUT_TICKET_DRAIN && clen == EFS_MD_PUT_TICKET_DRAIN_LEN);
    proposals++;
    if (step_down_at && proposals == (unsigned)step_down_at) {
        step_down_at = 0;
        return EFS_ERR_NOT_PRIMARY; /* leadership lost: not appended */
    }
    if (lose_before) { lose_before = 0; return EFS_ERR_IO; }
    if (keep_delayed) { keep_delayed = 0; memcpy(delayed, cmd, clen); } /* also commits later */
    commits++;
    int rc = apply_put_ticket_drain_cmd(h, cmd, clen, group);
    if (lose_after) { lose_after = 0; return EFS_ERR_IO; }
    return rc;
}
''' + scan_work + unpack + batch + drain_apply + pass_block + r'''
/* ---- storage members and the collector (as tests/test_put_orphan.c) ---- */
static struct efs_store *store[MEMBERS];
static uint32_t floor_[MEMBERS];
static struct efs_frag_id fid(const struct efs_put_ticket *t, unsigned fi)
{
    return (struct efs_frag_id){.export_id = 1, .ino = t->ino, .inode_generation = t->inode_gen,
                                .chunk_generation = t->object, .chunk_index = t->ci,
                                .fragment_index = fi, .coding_profile_id = t->coding_profile};
}
static void land(const struct efs_put_ticket *t)
{
    uint8_t data[32];
    for (unsigned fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        struct efs_frag_id id = fid(t, fi);
        memset(data, (int)fi, sizeof(data));
        assert(efs_store_put(store[t->nodes[fi] - 1], &id, data, sizeof(data), t->checksums[fi]) == EFS_OK);
    }
}
static unsigned present(const struct efs_put_ticket *t)
{
    unsigned mask = 0;
    for (unsigned fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        struct efs_frag_id id = fid(t, fi);
        uint8_t buf[64], sum[EFS_HASH_SIZE];
        uint32_t len = sizeof(buf);
        int ok = 0;
        if (efs_store_get(store[t->nodes[fi] - 1], &id, buf, &len, sum, &ok) == EFS_OK)
            mask |= 1u << fi;
    }
    return mask;
}
static uint32_t shard_of(const struct efs_put_ticket *t) { return efs_kv_lane_shard(t->ino, t->ci % EFS_META_LANES); }
static int c_revoked(void *ctx, const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch)
{ (void)ctx; return efs_session_barrier_done(host.kv, uuid, epoch); }
static int c_delete(void *ctx, const struct efs_put_ticket *t, unsigned fi)
{
    (void)ctx;
    unsigned m = t->nodes[fi] - 1;
    if (floor_[m] <= t->id.session_epoch) return EFS_ERR_BUSY;
    int rc = efs_session_local_revoked(host.kv, shard_of(t), t->id.client_uuid, t->id.session_epoch);
    if (rc == EFS_OK) rc = efs_put_ticket_orphan_get(host.kv, t, NULL);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    struct efs_frag_id id = fid(t, fi);
    rc = efs_store_del_if_sum(store[m], &id, t->checksums[fi]);
    return rc == EFS_ERR_EXIST ? EFS_ERR_PROTO : rc;
}
static int c_ack(void *ctx, const struct efs_put_ticket *t, uint32_t frags)
{ (void)ctx; return efs_put_ticket_orphan_ack(host.kv, t, frags); }
static const struct efs_put_orphan_ops orphan_ops = {NULL, c_revoked, c_delete, c_ack};
struct oscan { struct efs_put_ticket t[256]; uint32_t acked[256], n; };
static int oscan_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct oscan *o = u;
    assert(o->n < 256);
    assert(efs_put_ticket_orphan_decode(k, kl, v, vl, &o->t[o->n], &o->acked[o->n]) == EFS_OK);
    o->n++;
    return 0;
}
/* Collector passes over both groups' ledgers; returns records left. */
static unsigned collect(void)
{
    static struct oscan o;
    for (unsigned round = 0; round < 4; round++) {
        o.n = 0;
        for (uint32_t anchor = 1; anchor <= 2; anchor++) {
            uint8_t prefix[3];
            efs_put_ticket_orphan_prefix(anchor, prefix);
            assert(efs_kv_scan_prefix(host.kv, prefix, 3, oscan_cb, &o) == EFS_OK);
        }
        struct efs_put_orphan_stats st = {0};
        uint64_t skip = 0;
        for (uint32_t i = 0; i < o.n; i++)
            (void)efs_put_orphan_collect(host.kv, &o.t[i], o.acked[i], &orphan_ops, &skip, &st);
        if (!o.n) return 0;
    }
    return o.n;
}
static unsigned orphans(void)
{
    static struct oscan o;
    o.n = 0;
    for (uint32_t anchor = 1; anchor <= 2; anchor++) {
        uint8_t prefix[3];
        efs_put_ticket_orphan_prefix(anchor, prefix);
        assert(efs_kv_scan_prefix(host.kv, prefix, 3, oscan_cb, &o) == EFS_OK);
    }
    return o.n;
}
static int count_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{ (void)k; (void)kl; (void)v; (void)vl; (*(unsigned *)u)++; return 0; }
/* Live streams of one session in the index (both anchors). */
static unsigned streams_of(uint8_t uuid0)
{
    unsigned n = 0;
    for (uint32_t anchor = 1; anchor <= 2; anchor++) {
        uint8_t prefix[4];
        efs_put_ticket_stream_prefix(anchor, prefix);
        prefix[3] = uuid0;
        assert(efs_kv_scan_prefix(host.kv, prefix, 4, count_cb, &n) == EFS_OK);
    }
    return n;
}
/* Ticket rows of one session on one lane shard. */
static unsigned tickets_on(uint32_t shard, uint8_t uuid0)
{
    uint8_t prefix[4] = {(uint8_t)(shard >> 8), (uint8_t)shard, EFS_KV_KIND_PUT_TICKET, uuid0};
    unsigned n = 0;
    assert(efs_kv_scan_prefix(host.kv, prefix, 4, count_cb, &n) == EFS_OK);
    return n;
}
static void passes(void)
{
    for (uint8_t group = 0; group <= 2; group += 2)
        host_put_drain_pass(&host, group, group == 0 ? 1u : 2u);
}

/* ---- the revocation coordinator's transport, applied to the same KV ---- */
static unsigned rpcs;
static int fence_submit(void *ctx, const uint8_t *p, uint32_t n, struct efs_msg_raft_mkfs_reply *out)
{
    (void)ctx;
    rpcs++;
    memset(out, 0, sizeof(*out));
    out->leader_hint = 0;
    struct efs_kv *kv = host.kv;
    if (p[1] == EFS_MD_REQ_SESSION_APPEND_NEXT) { assert(n == 26); out->salt = 1; return out->rc = EFS_OK; }
    assert(p[1] == EFS_MD_CMD_SESSION && n >= 19);
    const uint8_t *uuid = p + 3;
    uint32_t f1 = n >= 23 ? rd32be(p + 19) : 0, f2 = n >= 27 ? rd32be(p + 23) : 0;
    struct efs_session_rec rec;
    switch (p[2]) {
    case EFS_MD_SESS_BEGIN: return out->rc = efs_session_begin_fence(kv, uuid);
    case 0:
        out->rc = efs_session_get(kv, uuid, &rec);
        if (out->rc) return out->rc;
        if (f1 >= EFS_SESSION_BITS) {
            for (unsigned i = 0; i < 8; i++)
                out->salt |= (uint64_t)rec.touched[(f1 - EFS_SESSION_BITS) * 8 + i] << (8 * i);
        } else
            out->salt = (uint64_t)rec.epoch | ((uint64_t)rec.state << 32);
        return EFS_OK;
    case EFS_MD_SESS_STORAGE_FENCE:
        for (unsigned m = 0; m < MEMBERS; m++) if (floor_[m] < f1) floor_[m] = f1;
        return out->rc = EFS_OK;
    case EFS_MD_SESS_FENCE_LOC: {
        /* [epoch][word][mask]: every shard of the mask (A11). */
        assert(n == 1 + EFS_MD_SESS_FENCE_LOC_LEN);
        uint64_t mask = (uint64_t)rd32be(p + 27) << 32 | rd32be(p + 31);
        out->rc = EFS_OK;
        for (unsigned b = 0; b < 64 && !out->rc; b++)
            if (mask & (1ull << b)) out->rc = efs_session_fence_local(kv, f2 * 64 + b, uuid, f1);
        return out->rc;
    }
    case EFS_MD_SESS_ACK:
        assert(n == 1 + EFS_MD_SESS_ACK_LEN);
        return out->rc = efs_session_ack_fence_word(kv, uuid, f1,
                                                    (uint64_t)rd32be(p + 23) << 32 | rd32be(p + 27));
    case EFS_MD_SESS_FINISH: return out->rc = efs_session_finish_fence_epoch(kv, uuid, f1);
    case EFS_MD_SESS_LEASE_LOCK_NEXT: out->salt = 1; return out->rc = EFS_OK;
    }
    assert(!"unexpected session request");
    return EFS_ERR_INVAL;
}
static void revoke(uint8_t uuid0)
{
    uint8_t uuid[16] = {uuid0};
    const struct efs_session_rpc rpc = {NULL, fence_submit, 0, NULL};
    struct efs_session_revoke_result res;
    int rc = efs_session_revoke(&rpc, uuid, &res);
    if (rc) fprintf(stderr, "revoke rc=%d phase=%s\n", rc, res.phase);
    assert(rc == EFS_OK && res.epoch == 2 && res.state == EFS_SESSION_ACTIVE && res.ntouch);
}

/* ---- fixture ---- */
static efs_ino_t ino;
static uint64_t gen;
static struct efs_meta_row row;
static struct efs_meta_pub pub(uint32_t ci, uint64_t seq, uint8_t uuid0, int durable)
{
    struct efs_meta_pub p = {0};
    assert(efs_meta_apply_get_inode(host.kv, ino, &row) == EFS_OK);
    p.ino = ino; p.inode_gen = gen; p.chunk_index = ci; p.new_size = 2ull * EFS_DEFAULT_CHUNK_SIZE;
    p.coding_profile_id = EFS_META_PROFILE_K2F1; p.fresh_object = 1; p.durable_result = durable;
    p.content_epoch = row.content_epoch; p.mtime_gen = row.mtime_gen; p.now = 10;
    p.lane_local = ci % EFS_META_LANES != 0;
    p.ticketed = 1; p.put_fragment_len = EFS_DEFAULT_CHUNK_SIZE / 2; p.put_member_mask = 7;
    p.put_id.client_uuid[0] = uuid0; p.put_id.session_epoch = 1; p.put_id.seq = seq;
    p.publication_id = p.put_id;
    p.candidate_gen = efs_meta_candidate_gen(p.put_id.client_uuid, 1, seq, ci, 0);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = i + 1;
        memset(p.ch.checksums[i], (int)(seq * 7 + i + 1), EFS_HASH_SIZE);
    }
    return p;
}
static struct efs_put_ticket ticket(uint32_t ci, uint64_t seq, uint8_t uuid0)
{
    struct efs_meta_pub p = pub(ci, seq, uuid0, 1);
    struct efs_put_ticket t;
    assert(efs_put_ticket_from_pub(&p, &t) == EFS_OK);
    return t;
}
static void session(uint8_t uuid0, unsigned lanes)
{
    uint8_t uuid[16] = {uuid0};
    assert(efs_session_create(host.kv, uuid, 1) == EFS_OK);
    for (uint8_t lane = 0; lane < lanes; lane++) {
        uint32_t sh = efs_kv_lane_shard(ino, lane);
        assert(efs_session_register(host.kv, uuid, 1, sh) == EFS_OK);
        assert(efs_session_establish(host.kv, sh, uuid, 1) == EFS_OK);
    }
}
static uint8_t cmd[3 + EFS_PUT_TICKET_BATCH_MAX * EFS_PUT_TICKET_WIRE_LEN];
static uint32_t frame(uint8_t tag, const struct efs_put_ticket *t, uint32_t n)
{
    cmd[0] = tag; cmd[1] = (uint8_t)(n >> 8); cmd[2] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) assert(efs_put_ticket_encode(&t[i], cmd + 3 + i * EFS_PUT_TICKET_WIRE_LEN) == EFS_OK);
    return 3 + n * EFS_PUT_TICKET_WIRE_LEN;
}
static uint32_t state(const struct efs_put_ticket *t)
{
    uint32_t st = 0;
    int rc = efs_put_ticket_state(host.kv, t, &st);
    return rc == EFS_ERR_NOT_FOUND ? 0 : (assert(rc == EFS_OK), st);
}
static struct efs_put_stream stream_of(const struct efs_put_ticket *t)
{
    struct efs_put_stream st;
    memcpy(st.uuid, t->id.client_uuid, 16);
    st.epoch = t->id.session_epoch;
    st.shard = shard_of(t);
    return st;
}
static uint32_t drain_cmd(const struct efs_put_ticket *t, uint8_t out[EFS_MD_PUT_TICKET_DRAIN_LEN])
{
    out[0] = EFS_MD_CMD_PUT_TICKET_DRAIN;
    memcpy(out + 1, t->id.client_uuid, 16);
    wr32be(out + 17, t->id.session_epoch);
    wr32be(out + 21, shard_of(t));
    return EFS_MD_PUT_TICKET_DRAIN_LEN;
}
static char dir[512];
static void reopen(void)
{
    efs_kv_lsm_close(host.kv);
    host.kv = efs_kv_lsm_open(dir, NULL);
    assert(host.kv);
}

/* 1. A client dies after ADMIT, before SETTLE. */
static void dead_client(void)
{
    const uint8_t U = 31;
    session(U, 3);
    uint8_t g = efs_raft_shard_group(efs_kv_lane_shard(ino, 0));
    assert(efs_raft_shard_group(efs_kv_lane_shard(ino, 2)) == g && efs_raft_shard_group(efs_kv_lane_shard(ino, 1)) != g);
    /* Streams A (lane 0: seq 1, 2, 5), B (lane 2: seq 3, 4), C (lane 1, the
     * other group: seq 6). */
    struct efs_put_ticket t[8];
    t[1] = ticket(0, 1, U); t[2] = ticket(64, 2, U); t[3] = ticket(2, 3, U);
    t[4] = ticket(66, 4, U); t[5] = ticket(128, 5, U); t[6] = ticket(1, 6, U); t[7] = ticket(130, 7, U);
    struct efs_put_ticket b[4] = {t[1], t[3], t[2], t[4]};
    assert(apply_put_ticket_batch_cmd(&host, cmd, frame(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 4), g) == EFS_OK);
    assert(efs_put_ticket_admit(host.kv, &t[5]) == EFS_OK && efs_put_ticket_admit(host.kv, &t[6]) == EFS_OK);
    for (unsigned i = 1; i <= 6; i++) { assert(state(&t[i]) == EFS_PUT_TICKET_ADMITTED); land(&t[i]); }
    struct efs_meta_pub p1 = pub(0, 1, U, 1);
    assert(efs_meta_apply_publish(host.kv, &p1) == EFS_OK && state(&t[1]) == EFS_PUT_TICKET_PUBLISHED);
    assert(efs_put_ticket_reject(host.kv, &t[4]) == EFS_OK); /* e.g. REPORT GONE */
    assert(streams_of(U) == 3);
    /* The client dies here: no SETTLE ever arrives. */

    /* Before revocation: nothing is drained or collected. */
    unsigned c0 = commits;
    passes();
    assert(commits == c0 && revoked_reads >= 2 && streams_of(U) == 3 && !orphans());
    uint8_t dc[EFS_MD_PUT_TICKET_DRAIN_LEN];
    assert(apply_put_ticket_drain_cmd(&host, dc, drain_cmd(&t[2], dc), g) == EFS_ERR_BUSY);
    assert(apply_put_ticket_drain_cmd(&host, dc, drain_cmd(&t[2], dc), (uint8_t)(2 - g)) == EFS_ERR_INVAL);
    dc[0] = EFS_MD_CMD_PUT_TICKET_DRAIN;
    assert(apply_put_ticket_drain_cmd(&host, dc, EFS_MD_PUT_TICKET_DRAIN_LEN - 1, g) == EFS_ERR_UNSUPPORTED);
    for (unsigned i = 1; i <= 6; i++) assert(present(&t[i]) == 7);

    /* An operator (or the departing mount) revokes the session. */
    revoke(U);
    /* Late K32 entries of the dead client apply as fenced no-ops. */
    struct efs_put_ticket late[2] = {t[3], t[4]};
    assert(apply_put_ticket_batch_cmd(&host, cmd, frame(EFS_MD_CMD_PUT_TICKET_SETTLE_BATCH, late, 2), g) == EFS_OK);
    assert(state(&t[3]) == EFS_PUT_TICKET_ADMITTED && state(&t[4]) == EFS_PUT_TICKET_REJECTED);
    struct efs_put_ticket again[2] = {t[5], t[7]};
    assert(apply_put_ticket_batch_cmd(&host, cmd, frame(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, again, 2), g) == EFS_OK);
    assert(state(&t[7]) == 0 && efs_put_ticket_admit(host.kv, &t[7]) == EFS_ERR_FENCED);

    /* The leaders drain every stream: the published head retires, every
     * unsettled body moves to the orphan ledger. */
    passes();
    assert(streams_of(U) == 0);
    for (unsigned lane = 0; lane < 3; lane++) assert(!tickets_on(efs_kv_lane_shard(ino, lane), U));
    for (unsigned i = 1; i <= 6; i++) assert(state(&t[i]) == EFS_PUT_TICKET_RETIRED);
    assert(efs_put_ticket_orphan(host.kv, &t[1]) == EFS_ERR_NOT_FOUND);
    for (unsigned i = 2; i <= 6; i++) assert(efs_put_ticket_orphan(host.kv, &t[i]) == EFS_OK);
    assert(efs_put_ticket_admit(host.kv, &t[2]) == EFS_ERR_FENCED);
    /* After revocation every body is reclaimed; the published one stays. */
    assert(collect() == 0);
    for (unsigned i = 2; i <= 6; i++) assert(present(&t[i]) == 0);
    assert(present(&t[1]) == 7);
    struct efs_meta_chunk ch;
    assert(efs_meta_apply_get_chunk(host.kv, ino, 0, &ch) == EFS_OK && ch.generation == t[1].object);
    puts("D31 dead client: ADMIT_BATCH/ADMIT then death; refused before revocation; late SETTLE_BATCH/ADMIT_BATCH fenced; "
         "both groups drained, published head retired, every unsettled body reclaimed, published body kept PASS");
}

/* 2. A pending K14 stage: the drain refuses there, then completes. */
static void staged(void)
{
    const uint8_t U = 32;
    session(U, 1);
    uint32_t ish = efs_kv_lane_shard(ino, 0);
    uint8_t g = efs_raft_shard_group(ish);
    struct efs_meta_pub sp = pub(192, 1, U, 0); /* lane 0, a fresh chunk */
    struct efs_put_ticket t1, t2 = ticket(256, 2, U);
    assert(efs_put_ticket_from_pub(&sp, &t1) == EFS_OK);
    assert(efs_put_ticket_admit(host.kv, &t1) == EFS_OK && efs_put_ticket_admit(host.kv, &t2) == EFS_OK);
    land(&t1); land(&t2);
    struct efs_txid tx = {{0}};
    tx.bytes[0] = 0x5A; tx.bytes[15] = 1;
    struct efs_txn_parts parts = {.n = 1, .shard = {ish}};
    struct efs_opid owner = sp.put_id;
    uint8_t key[EFS_KV_KEY_MAX], pay[EFS_PUB_STAGE_LEN];
    uint32_t kl = 0;
    assert(efs_meta_pub_stage_encode(&sp, &owner, pay) == EFS_OK);
    assert(efs_kv_key_chunk(ish, ino, gen, 0, 192, key, &kl) == EFS_OK);
    assert(efs_txn_apply_prepare(host.kv, EFS_TXN_PUBLISH, &tx, &parts, key, kl, pay, sizeof(pay)) == EFS_OK);
    /* The owner dies with the stage pending; it is revoked. */
    revoke(U);
    struct efs_put_stream st = stream_of(&t1);
    uint32_t moved = 9;
    assert(efs_put_ticket_drain(host.kv, &st, 8, &moved) == EFS_ERR_BUSY && !moved);
    unsigned c0 = commits;
    passes();
    assert(commits == c0 + 1 && streams_of(U) == 1 && state(&t1) == EFS_PUT_TICKET_ADMITTED);
    assert(efs_put_ticket_orphan(host.kv, &t1) == EFS_ERR_NOT_FOUND && !collect() && present(&t1) == 7);
    uint8_t dc[EFS_MD_PUT_TICKET_DRAIN_LEN];
    assert(apply_put_ticket_drain_cmd(&host, dc, drain_cmd(&t1, dc), g) == EFS_ERR_BUSY);
    /* The stage resolves (ABORT): the drain completes, the body goes. */
    assert(efs_txn_decide_parts(host.kv, efs_txn_coordinator(&tx, &parts), &tx, &parts, EFS_TXN_ABORT) == EFS_OK);
    assert(efs_meta_apply_txn_resolve(host.kv, &tx, ish, EFS_TXN_ABORT) == EFS_OK);
    passes();
    assert(streams_of(U) == 0 && state(&t1) == EFS_PUT_TICKET_RETIRED && state(&t2) == EFS_PUT_TICKET_RETIRED);
    assert(efs_put_ticket_orphan(host.kv, &t1) == EFS_OK && efs_put_ticket_orphan(host.kv, &t2) == EFS_OK);
    assert(collect() == 0 && !present(&t1) && !present(&t2));
    puts("D31 K14 stage: drain refuses while a stage may publish the ticket, completes after ABORT, bodies reclaimed PASS");
}

/* 3. A clean unmount settles, then revokes its own session. */
static void clean_unmount(void)
{
    const uint8_t U = 33;
    session(U, 2);
    struct efs_put_ticket t[6];
    for (unsigned i = 1; i <= 5; i++) {
        t[i] = ticket(i == 5 ? 1 : 64 * (i + 4), i, U); /* lane 0 stream; seq 5 on lane 1 */
        assert(efs_put_ticket_admit(host.kv, &t[i]) == EFS_OK);
        land(&t[i]);
    }
    /* seq 1 and 4 publish; seq 2 is abandoned (done, never published);
     * seq 3's PUT is still ambiguous at unmount (not done: never settled);
     * seq 5 published on its own stream. */
    for (unsigned i = 1; i <= 5; i += (i == 1 ? 3 : 1)) {
        struct efs_meta_pub p = pub(t[i].ci, i, U, 0); /* the REPORT path */
        assert(efs_meta_apply_publish(host.kv, &p) == EFS_OK);
    }
    /* The mount's last settle: in stream order, done tickets only. */
    assert(efs_put_ticket_settle(host.kv, &t[1]) == EFS_OK);
    assert(efs_put_ticket_settle(host.kv, &t[2]) == EFS_OK && efs_put_ticket_orphan(host.kv, &t[2]) == EFS_OK);
    assert(efs_put_ticket_settle(host.kv, &t[4]) == EFS_ERR_BUSY); /* behind seq 3 */
    assert(efs_put_ticket_settle(host.kv, &t[5]) == EFS_OK);
    assert(state(&t[3]) == EFS_PUT_TICKET_ADMITTED && state(&t[4]) == EFS_PUT_TICKET_PUBLISHED);
    /* Then it revokes its own session: no operator. */
    unsigned r0 = rpcs;
    revoke(U);
    assert(rpcs > r0);
    passes();
    assert(collect() == 0);
    /* No ADMITTED ticket and no leaked body; published bodies stay. */
    assert(streams_of(U) == 0 && !tickets_on(efs_kv_lane_shard(ino, 0), U) && !tickets_on(efs_kv_lane_shard(ino, 1), U));
    for (unsigned i = 1; i <= 5; i++) assert(state(&t[i]) == EFS_PUT_TICKET_RETIRED);
    assert(!present(&t[2]) && !present(&t[3]));
    assert(present(&t[1]) == 7 && present(&t[4]) == 7 && present(&t[5]) == 7);
    puts("D31 clean unmount: last settle, own-session revocation, drain and collection leave no ADMITTED ticket and no leaked body PASS");
}

/* 4. A leader change while a long stream drains. */
static void leader_change(void)
{
    const uint8_t U = 34;
    session(U, 1);
    enum { N = 150 };
    static struct efs_put_ticket t[N + 1];
    for (unsigned i = 1; i <= N; i++) {
        t[i] = ticket(64 * (i + 8), i, U);
        assert(efs_put_ticket_admit(host.kv, &t[i]) == EFS_OK);
        land(&t[i]);
    }
    revoke(U);
    uint8_t g = efs_raft_shard_group(shard_of(&t[1]));
    uint32_t anchor = g == 0 ? 1u : 2u;
    /* Old leader: the first proposal is lost before commit, the second
     * commits (and a delayed duplicate of it will commit later), its reply
     * is lost, then leadership goes. */
    unsigned c0 = commits;
    lose_before = 1;
    host_put_drain_pass(&host, g, anchor);
    assert(commits == c0 && streams_of(U) == 1 && state(&t[1]) == EFS_PUT_TICKET_ADMITTED);
    keep_delayed = 1; lose_after = 1;
    host_put_drain_pass(&host, g, anchor);
    assert(commits == c0 + 1 && state(&t[EFS_PUT_TICKET_DRAIN_MAX]) == EFS_PUT_TICKET_RETIRED &&
           state(&t[EFS_PUT_TICKET_DRAIN_MAX + 1]) == EFS_PUT_TICKET_ADMITTED);
    step_down_at = (int)proposals + 1;
    host_put_drain_pass(&host, g, anchor);
    assert(commits == c0 + 1 && streams_of(U) == 1);
    /* New leader: replays the log into its KV (reopened), cursor reset. */
    host.put_drain_cursor_len[gc_gi(g)] = 0;
    reopen();
    host_put_drain_pass(&host, g, anchor);
    assert(streams_of(U) == 0 && commits == c0 + 3);
    for (unsigned i = 1; i <= N; i++) assert(state(&t[i]) == EFS_PUT_TICKET_RETIRED);
    assert(orphans() == N);
    /* The old leader's delayed DRAIN commits after: a no-op. */
    assert(apply_put_ticket_drain_cmd(&host, delayed, sizeof(delayed), g) == EFS_OK);
    assert(orphans() == N && streams_of(U) == 0);
    reopen();
    assert(collect() == 0);
    for (unsigned i = 1; i <= N; i++) assert(!present(&t[i]));
    puts("D31 leader change: lost proposal, lost reply, step-down mid-pass, KV reopen, delayed duplicate DRAIN; "
         "150-ticket stream drained exactly once and reclaimed PASS");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    host.kv = efs_kv_lsm_open(dir, NULL);
    assert(host.kv);
    for (unsigned m = 0; m < MEMBERS; m++) assert((store[m] = efs_store_mem_create()));
    struct efs_meta_attrs a = {0, 0, 1};
    assert(efs_meta_apply_init(host.kv, 1) == EFS_OK);
    assert(efs_meta_apply_create_file(host.kv, &a, EFS_ROOT_INO, S_IFREG | 0600, "f", &ino) == EFS_OK);
    assert(efs_meta_apply_get_inode(host.kv, ino, &row) == EFS_OK);
    gen = row.generation;
    assert(efs_meta_apply_activate_lanes_at(host.kv, ino, 1ull << 1 | 1ull << 2, row.content_epoch) == EFS_OK);
    for (uint8_t lane = 1; lane <= 2; lane++)
        assert(efs_meta_apply_bootstrap_lane(host.kv, ino, gen, lane, row.content_epoch) == EFS_OK);
    assert(efs_meta_apply_get_inode(host.kv, ino, &row) == EFS_OK);
    dead_client();
    staged();
    clean_unmount();
    leader_change();
    assert(!counters.gc_scan_errors && !counters.put_orphan_failures);
    efs_kv_lsm_close(host.kv);
    for (unsigned m = 0; m < MEMBERS; m++) efs_store_mem_free(store[m]);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-drain-') as d:
    p = Path(d) / 'test.c'
    p.write_text(code)
    exe = p.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root / 'include'), str(p), str(root / 'libefs.a'), '-pthread', '-lm', '-ldl', '-libverbs', '-o', str(exe)], check=True)
    kv = Path(d) / 'kv'
    kv.mkdir()
    subprocess.run([str(exe), str(kv)], check=True, timeout=60)
