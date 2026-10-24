#!/usr/bin/env python3
"""K32: production ADMIT_BATCH / SETTLE_BATCH (src/server/raft_host.c) on a
real in-memory KV with the real ticket and session layer.

One entry admits many tickets of one Raft group across lane-shard streams,
each exactly as its own ADMIT would (idempotent on an admitted ticket); the
reply mask is read from the applied state, also after a lost reply (fresh
ReadIndex). SETTLE_BATCH settles several streams in one entry, in sequence
order per stream: a ticket that does not head its stream stays, and the
mask names exactly the retired tickets. Malformed frames (one ticket, too
many, two sessions, a repeated sequence, unsorted settle) and a ticket of
another group are refused before any proposal and by apply, which changes
nothing then. A group this host does not serve is forwarded unchanged."""
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
apply = fn('static int apply_put_ticket_batch_cmd(')
submit = fn('static void host_put_ticket_batch_submit(')
code = r'''
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/protocol.h"
#include "efs/put_ticket.h"
#include "efs/raft.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define HOST_SESS_REG_LEN 26
struct efs_raft_host { struct efs_kv *kv; };
static void wr32be(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (24 - 8 * i)); }
static int hosted = 1, propose_rc, lose_reply, read_rc;
static unsigned entries, forwards, establishes, reads;
static int host_hosts(struct efs_raft_host *h, uint8_t g) { (void)h; (void)g; return hosted; }
static int host_remote_cmd(struct efs_raft_host *h, uint8_t g, const uint8_t *c, uint32_t n,
                           struct efs_msg_raft_mkfs_reply *out, int rid)
{ (void)h; (void)g; (void)c; (void)n; (void)rid; forwards++; out->rc = EFS_OK; out->salt = 7; return EFS_OK; }
static int host_read_index(struct efs_raft_host *h, uint8_t g, int *hint) { (void)h; (void)g; (void)hint; reads++; return read_rc; }
'''+unpack+apply+r'''
static int host_session_establish_admit(struct efs_raft_host *h, const uint8_t *cmd, uint32_t n,
                                        uint8_t group, int *hint)
{
    (void)h; (void)hint;
    assert(n == HOST_SESS_REG_LEN && cmd[0] == EFS_MD_CMD_SESSION && cmd[1] == EFS_MD_SESS_ESTABLISH);
    uint32_t shard = (uint32_t)cmd[22] << 24 | (uint32_t)cmd[23] << 16 | (uint32_t)cmd[24] << 8 | cmd[25];
    assert(efs_raft_shard_group(shard) == group);
    establishes++;
    return EFS_OK;
}
static int host_propose_wait_idx(struct efs_raft_host *h, uint8_t group, const uint8_t *cmd,
                                 uint32_t n, int *hint, uint64_t *idx)
{
    (void)hint;
    if (propose_rc) return propose_rc;
    entries++;
    int rc = apply_put_ticket_batch_cmd(h, cmd, n, group);
    assert(rc == EFS_OK);
    if (lose_reply) { *idx = 0; return EFS_ERR_BUSY; }
    *idx = entries;
    return EFS_OK;
}
'''+submit+r'''
static struct efs_raft_host host;
static efs_ino_t ino;
static uint64_t gen;
static struct efs_put_ticket ticket(uint32_t ci, uint64_t seq, uint8_t uuid)
{
    struct efs_meta_pub p = {0};
    p.ino = ino; p.inode_gen = gen; p.chunk_index = ci; p.new_size = 64;
    p.coding_profile_id = EFS_META_PROFILE_K2F1; p.fresh_object = 1; p.durable_result = 1;
    p.ticketed = 1; p.put_fragment_len = 32; p.put_member_mask = 15;
    p.put_id.client_uuid[0] = uuid; p.put_id.session_epoch = 1; p.put_id.seq = seq;
    p.publication_id = p.put_id;
    p.candidate_gen = efs_meta_candidate_gen(p.put_id.client_uuid, 1, seq, ci, 0);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = i + 1;
        memset(p.ch.checksums[i], i + 1, EFS_HASH_SIZE);
    }
    struct efs_put_ticket t;
    assert(efs_put_ticket_from_pub(&p, &t) == EFS_OK);
    return t;
}
static uint32_t shard_of(const struct efs_put_ticket *t) { return efs_kv_lane_shard(t->ino, t->ci % EFS_META_LANES); }
static uint32_t frame(uint8_t tag, const struct efs_put_ticket *t, uint32_t n, uint8_t *cmd)
{
    cmd[0] = tag; cmd[1] = (uint8_t)(n >> 8); cmd[2] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) assert(efs_put_ticket_encode(&t[i], cmd + 3 + i * EFS_PUT_TICKET_WIRE_LEN) == EFS_OK);
    return 3 + n * EFS_PUT_TICKET_WIRE_LEN;
}
static uint8_t cmd[3 + (EFS_PUT_TICKET_BATCH_MAX + 1) * EFS_PUT_TICKET_WIRE_LEN];
static struct efs_msg_raft_mkfs_reply rpc(uint8_t tag, const struct efs_put_ticket *t, uint32_t n, uint8_t group)
{
    struct efs_msg_raft_mkfs_reply out;
    memset(&out, 0, sizeof(out));
    uint32_t len = frame(tag, t, n, cmd);
    host_put_ticket_batch_submit(&host, group, cmd, len, &out);
    return out;
}
static uint32_t state(const struct efs_put_ticket *t)
{
    uint32_t st = 0;
    int rc = efs_put_ticket_state(host.kv, t, &st);
    return rc == EFS_ERR_NOT_FOUND ? 0 : (assert(rc == EFS_OK), st);
}
int main(void)
{
    host.kv = efs_kv_mem_create();
    assert(host.kv);
    struct efs_meta_attrs a = {0, 0, 1};
    struct efs_meta_row row;
    assert(efs_meta_apply_init(host.kv, 1) == EFS_OK);
    assert(efs_meta_apply_create_file(host.kv, &a, EFS_ROOT_INO, S_IFREG | 0600, "f", &ino) == EFS_OK);
    assert(efs_meta_apply_get_inode(host.kv, ino, &row) == EFS_OK);
    gen = row.generation;
    uint8_t uuid[16] = {93};
    assert(efs_session_create(host.kv, uuid, 1) == EFS_OK);
    for (uint8_t lane = 0; lane < 8; lane++) {
        uint32_t sh = efs_kv_lane_shard(ino, lane);
        assert(efs_session_register(host.kv, uuid, 1, sh) == EFS_OK);
        assert(efs_session_establish(host.kv, sh, uuid, 1) == EFS_OK);
    }
    /* Lanes 0, 2, 4 share a group (odd stride: adjacent lanes alternate). */
    uint8_t g = efs_raft_shard_group(efs_kv_lane_shard(ino, 0));
    assert(efs_raft_shard_group(efs_kv_lane_shard(ino, 2)) == g && efs_raft_shard_group(efs_kv_lane_shard(ino, 1)) != g);
    /* Streams: A = lane 0 (seq 1, 2, 5), B = lane 2 (seq 3, 4). */
    struct efs_put_ticket t[6], b[EFS_PUT_TICKET_BATCH_MAX + 1];
    t[1] = ticket(0, 1, 93); t[2] = ticket(64, 2, 93); t[3] = ticket(2, 3, 93);
    t[4] = ticket(66, 4, 93); t[5] = ticket(128, 5, 93);

    /* One entry admits four tickets of two streams; one ESTABLISH per shard. */
    b[0] = t[1]; b[1] = t[3]; b[2] = t[2]; b[3] = t[4];
    struct efs_msg_raft_mkfs_reply r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 4, g);
    assert(r.rc == EFS_OK && r.salt == 0xF && entries == 1 && establishes == 2 && !reads);
    for (unsigned i = 1; i <= 4; i++) assert(state(&t[i]) == EFS_PUT_TICKET_ADMITTED);
    /* Admission is idempotent; a lost reply is proven by a fresh ReadIndex. */
    lose_reply = 1; b[0] = t[1]; b[1] = t[5];
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, g);
    assert(r.rc == EFS_OK && r.salt == 3 && entries == 2 && reads == 1);
    lose_reply = 0;
    /* A failed proof is an error, never a mask. */
    read_rc = EFS_ERR_NO_QUORUM; lose_reply = 1;
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, g);
    assert(r.rc == EFS_ERR_NO_QUORUM && !r.salt);
    read_rc = 0; lose_reply = 0;
    propose_rc = EFS_ERR_NO_QUORUM;
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, g);
    assert(r.rc == EFS_ERR_NO_QUORUM && !r.salt && entries == 3);
    propose_rc = 0;

    /* SETTLE_BATCH, sorted by (lane shard, sequence): A's seq 2 waits behind
     * its live seq 1 (and seq 5 behind it); B's 3 and 4 head their stream
     * and retire (ADMITTED: rejected, then retired). */
    uint32_t sa = shard_of(&t[1]), sb = shard_of(&t[3]);
    struct efs_put_ticket A[2] = {t[2], t[5]}, B[2] = {t[3], t[4]};
    if (sa < sb) { b[0] = A[0]; b[1] = A[1]; b[2] = B[0]; b[3] = B[1]; }
    else { b[0] = B[0]; b[1] = B[1]; b[2] = A[0]; b[3] = A[1]; }
    r = rpc(EFS_MD_CMD_PUT_TICKET_SETTLE_BATCH, b, 4, g);
    assert(r.rc == EFS_OK && entries == 4);
    assert(r.salt == (sa < sb ? 0xCu : 0x3u));
    int retired = 0;
    assert(!efs_put_ticket_retired(host.kv, &t[3], &retired) && retired);
    assert(!efs_put_ticket_retired(host.kv, &t[4], &retired) && retired);
    assert(!efs_put_ticket_retired(host.kv, &t[2], &retired) && !retired);
    assert(state(&t[2]) == EFS_PUT_TICKET_REJECTED || state(&t[2]) == EFS_PUT_TICKET_ADMITTED);
    /* The whole stream A in order now retires; B again is idempotent. */
    struct efs_put_ticket all[5] = {t[1], t[2], t[5], t[3], t[4]};
    if (sb < sa) { all[0] = t[3]; all[1] = t[4]; all[2] = t[1]; all[3] = t[2]; all[4] = t[5]; }
    r = rpc(EFS_MD_CMD_PUT_TICKET_SETTLE_BATCH, all, 5, g);
    assert(r.rc == EFS_OK && r.salt == 0x1F && entries == 5);

    /* Refusals before any proposal; apply refuses the same frames. */
    unsigned e0 = entries;
    struct efs_put_ticket fresh[4] = {ticket(4, 9, 93), ticket(4, 10, 93), ticket(1, 11, 93), ticket(4, 9, 93)};
    struct efs_put_ticket other = ticket(4, 12, 94);
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, fresh, 1, g); assert(r.rc == EFS_ERR_INVAL);
    b[0] = fresh[0]; b[1] = fresh[2];                         /* lane 1: other group */
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, g); assert(r.rc == EFS_ERR_INVAL);
    assert(apply_put_ticket_batch_cmd(&host, cmd, frame(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, cmd), g) == EFS_ERR_INVAL);
    b[0] = fresh[0]; b[1] = fresh[3];                         /* same sequence twice */
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, g); assert(r.rc == EFS_ERR_INVAL);
    b[0] = fresh[0]; b[1] = other;                            /* two sessions */
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, g); assert(r.rc == EFS_ERR_INVAL);
    assert(apply_put_ticket_batch_cmd(&host, cmd, frame(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, cmd), g) == EFS_ERR_UNSUPPORTED);
    b[0] = fresh[1]; b[1] = fresh[0];                         /* settle out of order */
    r = rpc(EFS_MD_CMD_PUT_TICKET_SETTLE_BATCH, b, 2, g); assert(r.rc == EFS_ERR_INVAL);
    for (unsigned i = 0; i <= EFS_PUT_TICKET_BATCH_MAX; i++) b[i] = ticket(4, 100 + i, 93);
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, EFS_PUT_TICKET_BATCH_MAX + 1, g); assert(r.rc == EFS_ERR_INVAL);
    assert(entries == e0 && state(&fresh[0]) == 0 && state(&fresh[1]) == 0);
    /* The largest batch is one entry. */
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, EFS_PUT_TICKET_BATCH_MAX, g);
    assert(r.rc == EFS_OK && r.salt == 0xFFFFFFFFull && entries == e0 + 1);
    /* Another host's group: forwarded unchanged, nothing proposed here. */
    hosted = 0; b[0] = fresh[0]; b[1] = fresh[1];
    r = rpc(EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH, b, 2, g);
    assert(r.rc == EFS_OK && r.salt == 7 && forwards == 1 && entries == e0 + 1);
    efs_kv_mem_free(host.kv);
    puts("K32 ADMIT_BATCH/SETTLE_BATCH: one entry per group batch, per-ticket admission and in-order per-stream settle, mask from applied state (lost reply via ReadIndex), refusals before proposal and at apply, forwarding PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-ticket-batch-') as d:
    p = Path(d) / 't.c'
    p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root / 'include'), str(p), str(root / 'libefs.a'), '-pthread', '-lm', '-ldl',
                   '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True, timeout=30)
