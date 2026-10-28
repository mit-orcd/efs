#!/usr/bin/env python3
"""K7/R2: the host decision sweeper reaches every live decision.

Compiles the production host_txn_decision_sweep, host_txn_retire,
pack_retire, pack_resolve and apply_txn_cmd from src/server/raft_host.c
against libefs.a and an on-disk KV LSM. host_bg_propose is the only
stand-in: a RESOLVE to the unavailable participant shard fails (its group
has no leader), any other RESOLVE succeeds, and a TXN_RETIRE is applied
through the production apply_txn_cmd on the shared store, which stands for
the coordinator group's replicated state.

Fixture: 40 live COMMIT decisions on one coordinator shard whose second
participant is unavailable, sorted first in key order (more than one
32-record page), then 20 resolvable ones. Before R2 every pass revisited
the same first page and the 20 never retired. Checked:
  1. one leader's passes retire all 20 and keep the 40 live, revisiting
     them on later rounds; the sweep keeps the shard armed while any live
     decision exists, and only a full clean wrap disarms it;
  2. a leadership change part way round (a second host, fresh cursor,
     same replicated store) completes the work;
  3. a cold restart (store closed and reopened, fresh host) does too;
  4. every verdict is still answered and no blocked decision is deleted.
Also checks the retention/recovery-age _Static_assert exists. Not covered:
a running Raft group or cluster (NOTRUN).
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'src/server/raft_host.c').read_text()

def function(head):
    a = s.index(head)
    while s.find(';', a) < s.find('{', a):
        a = s.index(head, a + 1)
    return s[a:s.index('\n}', a) + 2]

def define(name):
    return re.search(r'^#define %s\b.*$' % name, s, re.M).group(0) + '\n'

sa = s.index('_Static_assert(EFS_TXN_DECISION_RETAIN_NS')
static_assert = s[sa:s.index(';', sa) + 1] + '\n'
assert 'HOST_REC_AGE_NS' in static_assert

code = ''.join(define(n) for n in ('HOST_RETIRE_LEN', 'HOST_REC_AGE_NS', 'HOST_REC_MAX',
                                   'HOST_DEC_SCAN_WORK')) + r'''
#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include "efs/txn.h"
#include "efs/meta_apply.h" /* W77: RESOLVE/RETIRE apply through the publication stages */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
''' + static_assert + r'''
struct efs_raft_host {
    struct efs_kv *kv;
    int gc_running, running;
    uint64_t dec_retired, dec_retire_fail, dec_swept, dec_sweep_fail;
    struct efs_txn_decision_scan dec_scan[EFS_KV_SHARD_MASK + 1];
};
static uint64_t clock_ns;
static uint64_t now_ns(void) { return clock_ns; }
static void host_rec_mark(struct efs_raft_host *h, uint32_t shard) { (void)h; (void)shard; }
''' + '\n' + \
    function('static void wr64be(') + function('static uint64_t rd64be(') + r'''
#define COORD 21u
#define BLOCKED 22u
#define OKSH 23u
#define NBLOCK 40
#define NOK 20
static unsigned blocked_tries, resolves, retires;
static int host_bg_propose(struct efs_raft_host *h, uint8_t group, const uint8_t *cmd,
                           uint32_t clen, int verdict);
static int apply_txn_cmd(struct efs_raft_host *h, const uint8_t *cmd, uint32_t clen);
''' + function('static void pack_retire(') + function('static void pack_resolve(') + \
    function('static void host_txn_retire(') + function('static uint64_t txid_ns(') + \
    function('static uint32_t host_txn_decision_sweep(') + function('static int apply_txn_cmd(') + r'''
static int host_bg_propose(struct efs_raft_host *h, uint8_t group, const uint8_t *cmd,
                           uint32_t clen, int verdict)
{
    (void)verdict;
    if (cmd[0] == EFS_MD_CMD_RESOLVE) {
        assert(clen == 22);
        uint32_t sh = efs_load_be32(cmd + 17);
        assert(group == efs_raft_shard_group(sh));
        if (sh == BLOCKED) { blocked_tries++; return EFS_ERR_NOT_PRIMARY; }
        if (sh == OKSH) resolves++;
        return EFS_OK;
    }
    assert(cmd[0] == EFS_MD_CMD_TXN_RETIRE && group == efs_raft_shard_group(COORD));
    retires++;
    return apply_txn_cmd(h, cmd, clen); /* the coordinator group's apply */
}
static int cmp(const void *a, const void *b) { return memcmp(a, b, 16); }
static struct efs_txid ids[NBLOCK + NOK];
static void fixture(struct efs_kv *kv)
{
    struct efs_txn_parts pb = {.n = 2, .shard = {COORD, BLOCKED}};
    struct efs_txn_parts po = {.n = 2, .shard = {COORD, OKSH}};
    uint64_t born = 1000000000000ull, seq = 0;
    int i = 0;
    while (i < NBLOCK + NOK) {
        memcpy(ids[i].bytes, &born, 8); /* host-minted shape: time, then sequence */
        memcpy(ids[i].bytes + 8, &seq, 8);
        born += 1000; seq++;
        if (efs_txn_coordinator(&ids[i], &pb) == COORD && efs_txn_coordinator(&ids[i], &po) == COORD) i++;
    }
    qsort(ids, NBLOCK + NOK, sizeof(ids[0]), cmp);
    for (i = 0; i < NBLOCK + NOK; i++)
        assert(efs_txn_decide_parts(kv, COORD, &ids[i], i < NBLOCK ? &pb : &po, EFS_TXN_COMMIT) == EFS_OK);
}
static void counts(struct efs_kv *kv, uint32_t *live, uint32_t *retired)
{
    assert(efs_txn_decision_count(kv, COORD, live, retired) == EFS_OK);
}
static void verify(struct efs_kv *kv)
{
    uint32_t live, retired;
    counts(kv, &live, &retired);
    assert(live == NBLOCK && retired == NOK);
    for (int i = 0; i < NBLOCK + NOK; i++) {
        uint8_t key[EFS_KV_KEY_MAX], v[EFS_TXN_DECISION_LIVE_MAX];
        uint32_t kl = 0, vl = sizeof(v);
        int dec = 0;
        assert(efs_kv_key_decision(COORD, ids[i].bytes, key, &kl) == EFS_OK);
        assert((efs_kv_get(kv, key, kl, v, &vl) == EFS_OK) == (i < NBLOCK)); /* blocked stay live */
        assert(efs_txn_decision_get(kv, COORD, &ids[i], &dec) == EFS_OK && dec == EFS_TXN_COMMIT);
    }
}
static struct efs_raft_host *host(struct efs_kv *kv)
{
    struct efs_raft_host *h = calloc(1, sizeof(*h));
    assert(h);
    h->kv = kv; h->gc_running = h->running = 1;
    return h;
}
int main(int argc, char **argv)
{
    (void)argc;
    struct efs_kv_lsm_cfg cfg = {EFS_KV_LSM_NOSYNC, 0, 4, 0};
    char a[512], b[512], c[512];
    snprintf(a, sizeof(a), "%s/one", argv[1]);
    snprintf(b, sizeof(b), "%s/lead", argv[1]);
    snprintf(c, sizeof(c), "%s/cold", argv[1]);
    clock_ns = 1000000000000ull + 3600ull * 1000000000ull; /* all decisions an hour old */
    uint32_t live, retired, armed;

    /* 1. One leader. */
    struct efs_kv *kv = efs_kv_lsm_open(a, &cfg); assert(kv);
    fixture(kv);
    struct efs_raft_host *h = host(kv);
    {
        /* The old first-page behaviour retires nothing: a fresh cursor's
         * first page holds only blocked decisions. */
        struct efs_raft_host *probe = host(kv);
        assert(host_txn_decision_sweep(probe, COORD, clock_ns) != 0);
        counts(kv, &live, &retired); assert(live == NBLOCK + NOK && retired == 0 && !retires);
        free(probe);
    }
    for (int pass = 0; pass < 8; pass++) assert(host_txn_decision_sweep(h, COORD, clock_ns) != 0);
    verify(kv);
    assert(retires == NOK && resolves == NOK && h->dec_retired == NOK);
    unsigned tries = blocked_tries;
    for (int pass = 0; pass < 6; pass++) {
        armed = host_txn_decision_sweep(h, COORD, clock_ns);
        assert(armed); /* blocked live decisions keep the shard armed */
    }
    assert(blocked_tries > tries && h->dec_scan[COORD].wraps >= 3); /* retried every round */
    verify(kv);
    free(h); efs_kv_lsm_close(kv);

    /* 2. Leadership change part way round: same replicated store, new
     * leader with a fresh cursor. */
    retires = resolves = 0;
    kv = efs_kv_lsm_open(b, &cfg); assert(kv);
    fixture(kv);
    h = host(kv);
    assert(host_txn_decision_sweep(h, COORD, clock_ns)); /* first page only: all blocked */
    counts(kv, &live, &retired); assert(retired == 0);
    free(h);
    h = host(kv); /* the new leader */
    for (int pass = 0; pass < 6; pass++) assert(host_txn_decision_sweep(h, COORD, clock_ns));
    verify(kv);
    free(h); efs_kv_lsm_close(kv);

    /* 3. Cold restart part way round. */
    retires = 0;
    kv = efs_kv_lsm_open(c, &cfg); assert(kv);
    fixture(kv);
    h = host(kv);
    assert(host_txn_decision_sweep(h, COORD, clock_ns));
    free(h); efs_kv_lsm_close(kv);
    kv = efs_kv_lsm_open(c, &cfg); assert(kv);
    h = host(kv);
    for (int pass = 0; pass < 6; pass++) assert(host_txn_decision_sweep(h, COORD, clock_ns));
    verify(kv);
    assert(retires == NOK);

    /* 4. A shard whose live decisions all retire is disarmed only by a
     * clean page that covered the whole prefix from its start. */
    {
        struct efs_kv *mem = efs_kv_mem_create(); assert(mem);
        struct efs_raft_host *m = host(mem);
        struct efs_txn_parts po = {.n = 2, .shard = {COORD, OKSH}};
        assert(efs_txn_decide_parts(mem, COORD, &ids[NBLOCK], &po, EFS_TXN_COMMIT) == EFS_OK);
        assert(host_txn_decision_sweep(m, COORD, clock_ns) == 1); /* seen and retired */
        assert(host_txn_decision_sweep(m, COORD, clock_ns) == 0); /* full clean page */
        assert(host_txn_decision_sweep(m, 99, clock_ns) == 0);    /* never had any */
        free(m); efs_kv_mem_free(mem);
    }
    free(h); efs_kv_lsm_close(kv);
    puts("decision sweep: 40 blocked first, 20 later retire, blocked retried and kept live; "
         "same after a leadership change and a cold restart; retention >= 10x recovery age PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-decision-sweep-') as d:
    d = Path(d); cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE', '-I'+str(root/'include'), '-I'+str(root/'src/common')]
    (d/'t.c').write_text(code)
    subprocess.run(cc+flags+[str(d/'t.c'), str(root/'libefs.a'), '-pthread', '-lm', '-ldl', '-o', str(d/'t')], check=True)
    subprocess.run([str(d/'t'), str(d)], check=True, timeout=120)
