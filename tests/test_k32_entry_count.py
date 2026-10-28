#!/usr/bin/env python3
"""K32: Raft entries per 1 MiB multi-chunk write, before and after.

Part A runs the production K14/W77 publication code of
src/server/raft_host.c (host_pub_units_publish, host_pub_set_len,
host_pub_unit_commit, host_pub_unit_txn, the K29 TXN_BATCH builder,
host_txn_commit, host_pub_unit_abort) with the proposal, wait and finisher
layer replaced by a counter that records every Raft entry and decodes its
commands. One file, N sequential 1 MiB writes (8 chunks of 128 KiB each,
one W77 unit per write), reported U units per REPORT. "before" is the same
code with the K32 change undone in the slice: one transaction per unit and
one entry per PREPARE (the shape of devel cb970ca7c); "after" is this tree.

Part B runs the production K13 mount ticket ledger
(src/client/put_ticket_owner.c) with the RPCs replaced by a counter: each
write is one cohort of 8 chunk jobs on 8 threads, every ticket lands and is
confirmed, and a settle pass runs every P writes. "before" disables the
K32 admission batching and settle packing in the slice. The counted runs
compile a 20 s cohort linger: the sender's wait ends when all eight
members have queued, so the exact counts hold however the threads are
scheduled (with the production 1 ms bound a worker descheduled past it
under parallel load takes the single ADMIT: 65+64, the old flake). A
third run keeps the 1 ms linger and stalls one member per write 100 ms:
that member must be admitted alone by the single ADMIT (the sender did
not wait for it) and the rest still batch.

Checks: every set's PREPAREs name each chunk once plus one pin, one DECIDE,
every participant RESOLVEd once, one RETIRE; a STALE member republishes the
set unit by unit (only the loser is lost); a BUSY member stops the set; sets
break at another FileID and at a repeated chunk. The table is printed; the
"after" counts are asserted exactly and must be at most 2 entries per write
on the publication path for U >= 4.

Usage: make libefs.a && python3 tests/test_k32_entry_count.py
"""
from pathlib import Path
import os, shlex, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
if not (root / 'libefs.a').exists():
    sys.exit('build libefs.a first (make libefs.a)')
s = (root / 'src/server/raft_host.c').read_text()
def fn(head, src=None):
    src = src or s
    a = src.index(head)
    while src.find(';', a) < src.find('{', a):
        a = src.index(head, a + 1)
    return src[a:src.index('\n}', a) + 2] + '\n'
macros = s[s.index('#define HOST_PUBLISH_LEN'):s.index('#define HOST_ACTIVATE_AT_LEN')]
unpack = fn('static void unpack_publication(') + fn('static void unpack_report_ticket(')
packs = fn('static uint32_t pack_prep_raw(') + '#define HOST_DECIDE_MAX (23u + 4u * EFS_TXN_MAX_PART)\n' + \
    fn('static uint32_t pack_decide(') + fn('static void pack_resolve(')
cohort = s[s.index('#define HOST_CMD_COHORT_MAX'):s.index('/* DECIDE COMMIT at the coordinator')]
batch = s[s.index('#define HOST_TXN_BATCH_BYTES'):s.index('static int host_txn_commit(')] + fn('static int host_txn_commit(')
rawpack = fn('static int host_prep_raw_pack(')
w77 = s[s.index('/* ---- W77 · request-wide publication of one atomic unit'):s.index('void server_raft_host_report(')]
assert 'host_pub_units_publish' in w77 and 'host_pub_set_len' in w77
before = w77
for old, new in [('uint32_t nset = host_pub_set_len(u), m;', 'uint32_t nset = 1, m;'),
                 ('rc = host_txn_batch_add(h, &batch, efs_raft_shard_group(lsh), cmd, clen, hint);',
                  'rc = host_txn_batch_add(h, &batch, efs_raft_shard_group(lsh), cmd, clen, hint);\n'
                  '            if (rc == EFS_OK) rc = host_txn_batch_flush(h, &batch, hint);'),
                 ('rc = host_txn_batch_add(h, &batch, efs_raft_shard_group(ish), cmd, clen, hint);',
                  'rc = host_txn_batch_add(h, &batch, efs_raft_shard_group(ish), cmd, clen, hint);\n'
                  '        if (rc == EFS_OK) rc = host_txn_batch_flush(h, &batch, hint);')]:
    assert before.count(old) == 1, old
    before = before.replace(old, new)

server = r'''
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/protocol.h"
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "efs/txn.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define EFS_FAULTS 0
#define HOST_NGROUPS 2
struct efs_raft_host {
    pthread_mutex_t mu; struct efs_raft_disk *disk; struct efs_kv *kv;
    uint64_t obs_pub_units, obs_pub_unit_aborts, obs_pub_sets, obs_pub_set_splits;
};
struct host_idx_ref { uint8_t group; uint64_t idx; uint64_t term; };
static void wr32be(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (24 - 8 * i)); }
static void wr64be(uint8_t *p, uint64_t n) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(n >> (56 - 8 * i)); }
static uint32_t rd32be(const uint8_t *p) { uint32_t n = 0; for (unsigned i = 0; i < 4; i++) n = (n << 8) | p[i]; return n; }
static uint64_t rd64be(const uint8_t *p) { uint64_t n = 0; for (unsigned i = 0; i < 8; i++) n = (n << 8) | p[i]; return n; }
static uint64_t now_ns(void) { return 1700000000000000000ull; }
static uint64_t now_us_(void) { return now_ns() / 1000; }
static int raft_dbg_on(void) { return 0; }
static struct efs_raft *group_raft(struct efs_raft_host *h, uint8_t g) { (void)h; (void)g; return NULL; }
static int host_report_release(struct efs_raft_host *h, int held) { (void)h; (void)held; return EFS_OK; }
/* ---- the counted Raft layer ---- */
enum { PREP, DECIDE, RESOLVE, RETIRE, KINDS };
static unsigned ent[3][KINDS], cmds[3][KINDS];
static uint64_t next_idx;
static int verdict[1 << 16];
static int stale_ci = -1, busy_ci = -1, decide_abort;
/* chunk -> times staged in COMMITted transactions (pins: [4096]) */
static unsigned staged[8192], resolved_parts;
static struct efs_txid cur;
static int cur_have;
static int classify(const uint8_t *c, uint32_t n, uint64_t idx)
{
    int rc = EFS_OK;
    if (c[0] == EFS_MD_CMD_PREPARE) {
        uint32_t off = 19 + 4u * c[18];
        uint32_t klen = (uint32_t)c[off] << 8 | c[off + 1];
        const uint8_t *pay = c + off + 2 + klen;
        uint32_t plen = n - (off + 2 + klen);
        if (!cur_have) { memcpy(cur.bytes, c + 2, 16); cur_have = 1; }
        assert(!memcmp(cur.bytes, c + 2, 16));
        if (plen == EFS_PUB_STAGE_LEN) {
            int ci = (int)rd32be(pay + 9);
            assert(ci < 4096);
            staged[ci]++;
            if (ci == stale_ci) rc = EFS_ERR_STALE;
            if (ci == busy_ci) rc = EFS_ERR_BUSY;
        } else {
            assert(plen == EFS_PUB_PIN_LEN);
            staged[4096]++;
        }
        (void)idx;
        return rc;
    }
    if (c[0] == EFS_MD_CMD_RESOLVE) { resolved_parts++; return EFS_OK; }
    if (c[0] == EFS_MD_CMD_DECIDE) cur_have = 0; /* the next PREPAREs are a new transaction */
    return EFS_OK;
}
static int kind_of(uint8_t tag) { return tag == EFS_MD_CMD_PREPARE ? PREP : tag == EFS_MD_CMD_DECIDE ? DECIDE : RESOLVE; }
static void entry(uint8_t g, const uint8_t *cmd, uint32_t n, uint64_t *idx, uint64_t *term)
{
    uint64_t i = ++next_idx;
    int v = EFS_OK;
    assert(i < (1 << 16));
    if (idx) *idx = i;
    if (term) *term = 1;
    if (cmd[0] == EFS_MD_CMD_TXN_BATCH) {
        uint32_t k = (uint32_t)cmd[2] << 8 | cmd[3], at = 4;
        assert(k >= 2 && k <= EFS_MD_TXN_BATCH_MAX);
        ent[g][kind_of(cmd[1])]++;
        for (uint32_t j = 0; j < k; j++) {
            uint32_t len = (uint32_t)cmd[at] << 8 | cmd[at + 1];
            assert(cmd[at + 2] == cmd[1]);
            uint32_t sh = cmd[1] == EFS_MD_CMD_PREPARE ? rd32be(cmd + at + 2 + 2) : rd32be(cmd + at + 2 + 17);
            (void)sh;
            int one = classify(cmd + at + 2, len, i);
            if (one != EFS_OK && v == EFS_OK) v = one;
            cmds[g][kind_of(cmd[1])]++;
            at += 2 + len;
        }
        assert(at == n);
    } else {
        assert(cmd[0] == EFS_MD_CMD_PREPARE || cmd[0] == EFS_MD_CMD_DECIDE || cmd[0] == EFS_MD_CMD_RESOLVE);
        ent[g][kind_of(cmd[0])]++;
        cmds[g][kind_of(cmd[0])]++;
        v = classify(cmd, n, i);
        if (cmd[0] == EFS_MD_CMD_DECIDE && cmd[21] == EFS_TXN_ABORT) decide_abort++;
    }
    verdict[i] = v;
}
static int host_propose_mode(struct efs_raft_host *h, uint8_t g, const uint8_t *c, uint32_t n,
                             uint64_t *idx, uint64_t *term, int *hint, int local)
{ (void)h; (void)hint; (void)local; entry(g, c, n, idx, term); return EFS_OK; }
static int host_propose(struct efs_raft_host *h, uint8_t g, const uint8_t *c, uint32_t n,
                        uint64_t *idx, uint64_t *term, int *hint)
{ (void)h; (void)hint; entry(g, c, n, idx, term); return EFS_OK; }
static int host_propose_wait(struct efs_raft_host *h, uint8_t g, const uint8_t *c, uint32_t n, int *hint)
{ (void)h; (void)hint; uint64_t i; entry(g, c, n, &i, NULL); return verdict[i]; }
static int host_wait_refs(struct efs_raft_host *h, struct host_idx_ref *r, int n, int *hint)
{ (void)h; (void)hint; for (int i = 0; i < n; i++) assert(r[i].idx); return EFS_OK; }
static int host_apply_rc(struct efs_raft_host *h, uint8_t g, uint64_t idx, uint64_t term)
{ (void)h; (void)g; (void)term; return verdict[idx]; }
static void host_fin_add(struct efs_raft_host *h, const struct efs_txid *t, const struct efs_txn_parts *p,
                         uint32_t coord, int retire_only)
{ (void)h; (void)t; (void)p; assert(retire_only == 1); ent[efs_raft_shard_group(coord)][RETIRE]++; }
#define HOST_FIN_ABORT 2
''' + macros + unpack + packs + cohort + batch + rawpack + '@W77@' + r'''
static struct efs_raft_host host;
static efs_ino_t ino, ino2;
static struct efs_opid session;
static struct efs_report_pubid pubid;
static struct host_pub_unit *unit(efs_ino_t f, uint32_t ci0, uint32_t n)
{
    struct host_pub_unit *u = calloc(1, sizeof(*u));
    assert(u && n <= EFS_PUB_UNIT_MAX);
    u->ino = f; u->ci0 = ci0; u->n = n;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *c = u->cmd[i];
        c[0] = EFS_MD_CMD_PUBLISH;
        wr64be(c + 1, f);
        wr32be(c + 9, ci0 + i);
        wr64be(c + HOST_PUBLISH_LEN, 1000 + ci0 + i); /* ticket sequence */
        u->group[i] = efs_raft_shard_group(efs_kv_lane_shard(f, (uint8_t)((ci0 + i) % EFS_META_LANES)));
    }
    return u;
}
static void reset(void) { memset(ent, 0, sizeof(ent)); memset(cmds, 0, sizeof(cmds)); memset(staged, 0, sizeof(staged));
    resolved_parts = 0; decide_abort = 0; cur_have = 0; }
static unsigned total(void) { unsigned t = 0; for (int g = 0; g < 3; g++) for (int k = 0; k < KINDS; k++) t += ent[g][k]; return t; }
/* N 1 MiB writes (8 chunks each), U units per REPORT: entries. */
static unsigned run(unsigned writes, unsigned per_report)
{
    reset();
    unsigned all = 0;
    for (unsigned w = 0; w < writes; w += per_report) {
        struct host_pub_unit *head = NULL, **tail = &head;
        for (unsigned k = w; k < w + per_report && k < writes; k++) {
            *tail = unit(ino, 8 * k, 8);
            tail = &(*tail)->next;
        }
        uint32_t lost = 0;
        int hint = -1;
        cur_have = 0;
        assert(host_pub_units_publish(&host, head, 1, &session, &pubid, &lost, &hint) == EFS_OK && !lost);
    }
    for (unsigned c = 0; c < 8 * writes; c++) assert(staged[c] == 1);
    all = total();
    return all;
}
int main(int argc, char **argv)
{
    int after = argc > 1 && !strcmp(argv[1], "after");
    host.kv = efs_kv_mem_create();
    assert(host.kv);
    struct efs_meta_attrs a = {0, 0, 1};
    assert(efs_meta_apply_init(host.kv, 1) == EFS_OK);
    assert(efs_meta_apply_create_file(host.kv, &a, EFS_ROOT_INO, S_IFREG | 0600, "f", &ino) == EFS_OK);
    assert(efs_meta_apply_create_file(host.kv, &a, EFS_ROOT_INO, S_IFREG | 0600, "g", &ino2) == EFS_OK);
    session.client_uuid[0] = 93; session.session_epoch = 1;
    pubid.mint_ns = now_ns(); pubid.seq = 7;
    const unsigned N = 64, U[] = {1, 2, 4, 8, 16, 32, 64};
    for (unsigned i = 0; i < sizeof(U) / sizeof(U[0]); i++) {
        unsigned e = run(N, U[i]);
        printf("A %s U=%u writes=%u entries=%u g0=%u+%u+%u+%u g2=%u+%u+%u+%u per_write=%.3f\n",
               after ? "after" : "before", U[i], N, e,
               ent[0][PREP], ent[0][DECIDE], ent[0][RESOLVE], ent[0][RETIRE],
               ent[2][PREP], ent[2][DECIDE], ent[2][RESOLVE], ent[2][RETIRE], (double)e / N);
        unsigned reports = N / U[i];
        if (!after) {
            /* Pre-K32: per unit 8 stage + 1 pin PREPARE, DECIDE, one RESOLVE per group, RETIRE. */
            assert(e == 13 * N);
        } else {
            /* Per REPORT (one set): PREPAREs in ceil(stages per group / 96) entries
             * per group (the pin rides the inode group's), DECIDE, one RESOLVE per
             * group, RETIRE. */
            unsigned st = 8 * U[i], ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
            unsigned prep = 0;
            for (unsigned g = 0; g <= 2; g += 2) {
                unsigned c = st / 2 + (g == ig);
                prep += (c + EFS_MD_TXN_BATCH_MAX - 1) / EFS_MD_TXN_BATCH_MAX;
            }
            assert(e == reports * (prep + 1 + 2 + 1));
            assert(cmds[0][PREP] + cmds[2][PREP] == N * 8 + reports);
            if (U[i] >= 4) assert((double)e / N <= 2.0);
        }
    }
    if (!after) return 0;
    /* STALE member: the set aborts and republishes unit by unit; only the
     * loser is lost. */
    reset();
    struct host_pub_unit *head = NULL, **tail = &head;
    for (unsigned k = 0; k < 8; k++) { *tail = unit(ino, 1000 + 8 * k, 8); tail = &(*tail)->next; }
    stale_ci = 1000 + 8 * 3 + 5;
    uint32_t lost = 0; int hint = -1; cur_have = 0;
    assert(host_pub_units_publish(&host, head, 1, &session, &pubid, &lost, &hint) == EFS_OK && lost == 1);
    assert(decide_abort == 2 && host.obs_pub_set_splits == 1);
    stale_ci = -1;
    /* BUSY member: the set stops with BUSY, nothing republished. */
    reset(); head = NULL; tail = &head;
    for (unsigned k = 0; k < 4; k++) { *tail = unit(ino, 2000 + 8 * k, 8); tail = &(*tail)->next; }
    busy_ci = 2000 + 3;
    lost = 0; cur_have = 0;
    assert(host_pub_units_publish(&host, head, 1, &session, &pubid, &lost, &hint) == EFS_ERR_BUSY && !lost);
    assert(decide_abort == 1 && cmds[0][PREP] + cmds[2][PREP] == 4 * 8 + 1);
    busy_ci = -1;
    /* Sets break at another FileID and at a repeated chunk; go=0 frees only. */
    struct host_pub_unit *x = unit(ino, 3000, 8), *y = unit(ino, 3008, 8), *z = unit(ino2, 0, 8), *q = unit(ino2, 4, 8);
    x->next = y; y->next = z; z->next = q;
    assert(host_pub_set_len(x) == 2 && host_pub_set_len(z) == 1);
    reset(); lost = 0; cur_have = 0;
    assert(host_pub_units_publish(&host, x, 1, &session, &pubid, &lost, &hint) == EFS_OK && !lost);
    assert(ent[0][DECIDE] + ent[2][DECIDE] == 3);
    x = unit(ino, 0, 8);
    assert(host_pub_units_publish(&host, x, 0, &session, &pubid, &lost, &hint) == EFS_OK && !lost);
    /* A set holds at most EFS_PUB_STAGE_RESOLVE_MAX stages per lane shard. */
    head = NULL; tail = &head;
    for (unsigned k = 0; k < 70; k++) { *tail = unit(ino, 64 * k, 2); tail = &(*tail)->next; }
    assert(host_pub_set_len(head) == EFS_PUB_STAGE_RESOLVE_MAX);
    while (head) { x = head->next; free(head); head = x; }
    printf("A checks: chunk once per set, one pin/DECIDE/RETIRE, STALE splits to units, BUSY stops, set breaks PASS\n");
    efs_kv_mem_free(host.kv);
    return 0;
}
'''
o = (root / 'src/client/put_ticket_owner.c').read_text()
o_before = o
for old, new in [('if (rc == EFS_OK && admit_batched(out))', 'if (0 && admit_batched(out))'),
                 ('total + g_pass[order[b]].n <= EFS_PUT_TICKET_BATCH_MAX; b++)', '0; b++)')]:
    assert o_before.count(old) == 1, old
    o_before = o_before.replace(old, new)
client = r'''
#include "client_internal.h"
#include "efs/put_ticket.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct efs_client g_client;
static uint64_t next_seq = 1;
static pthread_mutex_t mock_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned admit_e[3], settle_e[3];
static unsigned char single_ci[4096], batched_ci[4096];
int efs_client_rpc_past_deadline(void) { return 0; }
static __thread uint64_t deadline;
uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
void efs_client_rpc_set_deadline_ms(uint64_t d) { deadline = d; }
int efs_client_put_id(struct efs_opid *out)
{ pthread_mutex_lock(&mock_mu); memset(out, 0, sizeof(*out)); out->client_uuid[0] = 9; out->session_epoch = 1;
  out->seq = next_seq++; pthread_mutex_unlock(&mock_mu); return EFS_OK; }
int efs_client_rpc_getchunks_fileid(efs_export_id_t e, efs_ino_t ino, uint64_t g, uint32_t start,
                                    struct efs_chunk_rec *r, uint32_t *n, uint64_t *resolved)
{ (void)e; (void)g; (void)start; (void)r; (void)n; (void)ino; *resolved = 7; return EFS_OK; }
static uint8_t grp(const struct efs_put_ticket *t) { return efs_raft_shard_group(efs_kv_lane_shard(t->ino, t->ci % EFS_META_LANES)); }
int efs_client_rpc_put_ticket(const struct efs_put_ticket *t, int probe, uint32_t *state)
{ assert(!probe); pthread_mutex_lock(&mock_mu); admit_e[grp(t)]++; single_ci[t->ci]++; pthread_mutex_unlock(&mock_mu);
  *state = EFS_PUT_TICKET_ADMITTED; return EFS_OK; }
int efs_client_session_admit(uint32_t shard, const struct efs_opid *id) { (void)shard; (void)id; return EFS_OK; }
int efs_client_rpc_put_ticket_settle(const struct efs_put_ticket *t, uint32_t n, uint32_t *retired)
{ pthread_mutex_lock(&mock_mu); settle_e[grp(t)]++; pthread_mutex_unlock(&mock_mu); *retired = n; return EFS_OK; }
int efs_client_rpc_put_ticket_batch(const struct efs_put_ticket *t, uint32_t n, int settle, uint32_t *mask)
{ pthread_mutex_lock(&mock_mu); (settle ? settle_e : admit_e)[grp(t)]++;
  for (uint32_t i = 0; i < n && !settle; i++) batched_ci[t[i].ci]++;
  pthread_mutex_unlock(&mock_mu);
  for (uint32_t i = 1; i < n; i++) assert(grp(&t[i]) == grp(t));
  *mask = (uint32_t)((1ull << n) - 1); return EFS_OK; }
void efs_client_stripe_closed(const struct efs_put_ticket *t){assert(t && t->object);}
int efs_client_put_referenced(efs_ino_t ino, uint32_t ci, uint64_t object) { (void)ino; (void)ci; (void)object; return 0; }
#include "OWNER"
static const efs_node_id_t nodes[3] = {1, 2, 3};
static uint8_t cks[3][EFS_HASH_SIZE];
static struct efs_put_admit_cohort cohort;
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static unsigned entered;
static int late; /* stall the last chunk of every write past the linger */
struct job { uint32_t ci; struct efs_put_ticket t; };
static void *job_main(void *arg)
{
    struct job *j = arg;
    efs_client_put_cohort_enter(&cohort);
    pthread_mutex_lock(&gate_mu); entered++; pthread_cond_broadcast(&gate_cv);
    while (entered < 8) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
    if (late && j->ci % 8 == 7) usleep(100000);
    assert(efs_client_put_admit(4097, j->ci, nodes, (const uint8_t (*)[EFS_HASH_SIZE])cks, 32, 0, 0, &j->t) == EFS_OK);
    efs_client_put_cohort_leave();
    return NULL;
}
int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    late = argc == 3 && !strcmp(argv[2], "late");
    unsigned N = late ? 8 : 64, P = (unsigned)atoi(argv[1]);
    g_client.node_count = 4; g_client.export_id = 1;
    for (unsigned i = 0; i < 4; i++) g_client.nodes[i].id = i + 1;
    for (unsigned w = 0; w < N; w++) {
        struct job jobs[8]; pthread_t th[8]; entered = 0;
        for (unsigned i = 0; i < 8; i++) { jobs[i].ci = 8 * w + i; assert(!pthread_create(&th[i], NULL, job_main, &jobs[i])); }
        for (unsigned i = 0; i < 8; i++) pthread_join(th[i], NULL);
        for (unsigned i = 0; i < 8; i++) efs_client_put_release(jobs[i].t.id.seq, 1);
        if ((w + 1) % P == 0) assert(efs_client_put_settle() == EFS_OK);
    }
    assert(!efs_client_put_outstanding());
    if (late) {
        unsigned singles = 0;
        for (unsigned c = 0; c < 8 * N; c++) {
            assert(single_ci[c] + batched_ci[c] >= 1);
            singles += single_ci[c];
            if (c % 8 == 7) assert(single_ci[c] == 1 && !batched_ci[c]);
        }
        printf("B late linger_us=%u writes=%u late_single=%u singles=%u admit=%u+%u\n", EFS_PUT_ADMIT_LINGER_US,
               N, N, singles, admit_e[0], admit_e[2]);
        return 0;
    }
    printf("B %s P=%u writes=%u admit=%u+%u settle=%u+%u per_write=%.3f+%.3f\n", MODE, P, N,
           admit_e[0], admit_e[2], settle_e[0], settle_e[2],
           (double)(admit_e[0] + admit_e[2]) / N, (double)(settle_e[0] + settle_e[2]) / N);
    return 0;
}
'''
LINGER = ['-DEFS_PUT_ADMIT_LINGER_US=20000000u']
def build_run(d, name, src, defs, args_list, libs):
    p = Path(d) / (name + '.c')
    p.write_text(src)
    exe = p.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-I' + str(root / 'include'), '-I' + str(root / 'src/client')] + defs +
                   [str(p)] + libs + ['-pthread', '-lm', '-ldl', '-o', str(exe)], check=True)
    out = []
    for a in args_list:
        out += subprocess.run([str(exe)] + a, check=True, timeout=60, capture_output=True,
                              text=True).stdout.splitlines()
    return out
lib = [str(root / 'libefs.a')]
rows = []
with tempfile.TemporaryDirectory(prefix='k32-entry-count-') as d:
    rows += build_run(d, 'a_before', server.replace('@W77@', before), [], [['before']], lib)
    rows += build_run(d, 'a_after', server.replace('@W77@', w77), [], [['after']], lib)
    (Path(d) / 'owner_before.c').write_text(o_before)
    (Path(d) / 'owner_after.c').write_text(o)
    for mode in ('before', 'after'):
        rows += build_run(d, 'b_' + mode, client.replace('OWNER', str(Path(d) / ('owner_%s.c' % mode))),
                          ['-DMODE="%s"' % mode] + LINGER, [['8'], ['32']], lib)
    rows += build_run(d, 'b_late', client.replace('OWNER', str(Path(d) / 'owner_after.c')),
                      ['-DMODE="late"'], [['8', 'late']], lib)
for r in rows:
    print(r)
b = {}
for r in rows:
    if r.startswith('B ') and not r.startswith('B late '):
        f = dict(x.split('=', 1) for x in r.split()[2:])
        b[(r.split()[1], f['P'])] = f
# Admission: one entry per ticket before; one ADMIT_BATCH per group per write after.
assert b[('before', '8')]['admit'] == '256+256' and b[('after', '8')]['admit'] == '64+64'
# Settle: one entry per stream per pass before; per group, 32 tickets per entry after.
assert b[('before', '32')]['settle'] == '64+64' and b[('after', '32')]['settle'] == '8+8'
assert b[('before', '8')]['settle'] == '256+256' and b[('after', '8')]['settle'] == '8+8'
a = {}
for r in rows:
    if r.startswith('A ') and ' U=' in r:
        f = dict(x.split('=', 1) for x in r.split()[2:])
        a[(r.split()[1], f['U'])] = float(f['per_write'])
def per_write(mode, u, p):
    f = b[(mode, p)]
    return a[(mode, u)] + float(f['per_write'].split('+')[0]) + float(f['per_write'].split('+')[1])
for u in ('1', '8', '64'):
    print('total U=%s P=32: before %.3f after %.3f entries per 1 MiB write' %
          (u, per_write('before', u, '32'), per_write('after', u, '32')))
assert per_write('before', '8', '32') == 23.0 and per_write('after', '8', '32') == 3.0
# Production 1 ms linger: a member past it is admitted alone (C asserts it).
late = [r for r in rows if r.startswith('B late ')]
assert len(late) == 1 and 'linger_us=1000 ' in late[0], late
print('K32 entry count: publication sets and TXN_BATCH PREPAREs, cohort ADMIT_BATCH, 1 ms linger bound with single-ADMIT fallback, multi-stream SETTLE_BATCH PASS')
