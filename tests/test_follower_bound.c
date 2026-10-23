/* K22/W23: universal follower memory/lag bound.
 *
 * Three in-process Raft replicas (memory log stores, queued delivery).
 * Replica 2 is a follower whose state machine cannot keep up:
 *   - sim arm (every build): its apply refuses EFS_ERR_BUSY while parked;
 *   - stalled-compactor arm (EFS_FAULTS=1 builds): its apply writes into a
 *     real LSM KV whose compactor is parked by the W23 fault hook
 *     (EFS_FAULT_COMPACT_STALL + fault file), with a 1 MiB L0 cap, so L0
 *     fills, flushes defer, the memtable reaches its derived budget and
 *     efs_kv_lsm_apply_room defers apply.
 * Asserts: the follower refuses AppendEntries busy (no silent drop), its
 * unapplied log never exceeds the cap, its KV memtable stays at the budget,
 * the healthy quorum keeps committing, and after the stall clears the
 * follower applies every committed entry. A third arm checks that a leader
 * at its own cap refuses proposals with EFS_ERR_BUSY and recovers.
 * Review S1 (Oct 22) arms: three voters restarted with identical 96-entry
 * uncommitted tails at cap 128 campaign (the counting arm is the former
 * scripts/reviews/k22_capacity_tail.c reproducer), elect a leader with
 * messages delivered, commit and apply the tail, and resume admission; the
 * leader does not step down for its own uncommitted tail. A committed but
 * unapplied backlog still holds campaigning until it applies. */
#include "efs/raft.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_tmpdir.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, m); failures++; } } while (0)

#define NR 3
#define QMAX 4096

struct qmsg { struct efs_raft_msg *m; };
struct net {
    struct efs_raft *r[NR];
    struct qmsg q[QMAX];
    unsigned head, tail;
    uint64_t busy_rejects_seen, data_to_busy;
    unsigned down;    /* replicas cut off: nothing to or from them */
    int drop_ae_rep;  /* lose every AppendEntries reply */
};

struct app {
    int park;              /* sim: refuse apply */
    struct efs_kv *kv;     /* stalled-compactor arm */
    uint64_t applied;
};

static struct efs_raft_msg *deep_copy(const struct efs_raft_msg *src)
{
    struct efs_raft_msg *m = malloc(sizeof(*m));
    if (!m) return NULL;
    *m = *src;
    m->wire = NULL;
    m->wire_len = 0;
    for (uint32_t i = 0; i < m->nentries; i++) {
        uint8_t *c = malloc(src->entries[i].clen ? src->entries[i].clen : 1);
        if (c && src->entries[i].clen)
            memcpy(c, src->entries[i].cmd, src->entries[i].clen);
        m->entries[i].cmd = c;
    }
    return m;
}

static void deep_free(struct efs_raft_msg *m)
{
    if (!m) return;
    for (uint32_t i = 0; i < m->nentries; i++) free((void *)m->entries[i].cmd);
    free(m);
}

static int q_send(void *net, const struct efs_raft_msg *msg)
{
    struct net *n = net;
    if (n->tail - n->head >= QMAX) return EFS_ERR_AGAIN;
    if (msg->type == EFS_RAFT_MSG_AE_REP && msg->busy && !msg->success)
        n->busy_rejects_seen++;
    n->q[n->tail++ % QMAX].m = deep_copy(msg);
    return EFS_OK;
}

static void deliver(struct net *n)
{
    while (n->head != n->tail) {
        struct efs_raft_msg *m = n->q[n->head++ % QMAX].m;
        if (m && m->to >= 0 && m->to < NR && n->r[m->to] &&
            !(n->down & (1u << m->to)) &&
            !(m->from >= 0 && m->from < NR && (n->down & (1u << m->from))) &&
            !(n->drop_ae_rep && m->type == EFS_RAFT_MSG_AE_REP))
            (void)efs_raft_recv(n->r[m->to], m);
        deep_free(m);
    }
}

static int app_apply(void *a_, uint64_t index, uint64_t term,
                     const uint8_t *cmd, uint32_t clen)
{
    struct app *a = a_;
    (void)term;
    if (a->park) return EFS_ERR_BUSY;
    if (a->kv && clen > 16) {
        int rc = efs_kv_lsm_apply_room(a->kv, 0);
        if (rc != EFS_OK) return rc;
        rc = efs_kv_put(a->kv, cmd, 16, cmd + 16, clen - 16);
        if (rc != EFS_OK) return rc;
    }
    a->applied = index;
    return EFS_OK;
}

static struct efs_raft_store *st[NR];
static struct app apps[NR];

static void boot(struct net *n, uint64_t cap)
{
    struct efs_raft_cfg cfg;
    memset(n, 0, sizeof(*n));
    memset(apps, 0, sizeof(apps));
    for (int i = 0; i < NR; i++) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.id = i;
        cfg.n = NR;
        cfg.election_ticks = 10;
        cfg.heartbeat_ticks = 2;
        cfg.boot_id = 1;
        cfg.rng_seed = 7 + (uint64_t)i;
        st[i] = efs_raft_mem_create();
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.send = q_send;
        cfg.net = n;
        cfg.apply = app_apply;
        cfg.app = &apps[i];
        n->r[i] = efs_raft_new(&cfg);
        CHECK(n->r[i] != NULL, "raft new");
        efs_raft_set_max_unapplied(n->r[i], cap);
    }
    /* Replica 0 leads; 1 and 2 never campaign. */
    efs_raft_allow_campaign(n->r[1], 0);
    efs_raft_allow_campaign(n->r[2], 0);
    for (int t = 0; t < 200 && efs_raft_role(n->r[0]) != EFS_RAFT_LEADER; t++) {
        for (int i = 0; i < NR; i++) (void)efs_raft_tick(n->r[i]);
        deliver(n);
    }
    CHECK(efs_raft_role(n->r[0]) == EFS_RAFT_LEADER, "replica 0 leads");
}

static void shutdown_net(struct net *n)
{
    deliver(n);
    for (int i = 0; i < NR; i++) {
        efs_raft_free(n->r[i]);
        efs_raft_mem_free(st[i]);
        n->r[i] = NULL;
    }
}

static void tick_all(struct net *n)
{
    for (int i = 0; i < NR; i++) (void)efs_raft_tick(n->r[i]);
    deliver(n);
}

static uint64_t unapplied(struct efs_raft *r)
{
    struct efs_raft_log_progress lp;
    if (efs_raft_log_progress(r, &lp) != EFS_OK) return UINT64_MAX;
    return lp.unapplied;
}

static int propose_one(struct net *n, uint64_t seq, uint32_t vlen)
{
    uint8_t cmd[16 + 4096];
    uint64_t idx = 0;
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x7a; /* not a Raft config command */
    for (int b = 0; b < 8; b++) cmd[8 + b] = (uint8_t)(seq >> (56 - 8 * b));
    memset(cmd + 16, (int)(seq & 0xff), vlen);
    int rc = efs_raft_propose(n->r[0], cmd, 16 + vlen, &idx);
    deliver(n);
    return rc;
}

/* Common driver: write `total` entries, follower 2 stalled; then release
 * and require full catch-up. stall_release() clears the stall. */
static void drive(struct net *n, uint64_t cap, int total, uint32_t vlen,
                  void (*release)(void), struct efs_kv *kv, uint64_t kv_mem_bound,
                  uint64_t kv_l0_bound)
{
    uint64_t peak_unapplied = 0, peak_mt = 0, peak_l0 = 0;
    int refused = 0;

    for (int i = 0; i < total; i++) {
        int rc = propose_one(n, (uint64_t)i, vlen);
        if (rc != EFS_OK) refused++;
        if (i % 8 == 0) tick_all(n);
        uint64_t u = unapplied(n->r[2]);
        if (u > peak_unapplied) peak_unapplied = u;
        if (kv) {
            struct efs_kv_lsm_stats ks;
            if (efs_kv_lsm_stats(kv, &ks) == EFS_OK) {
                if (ks.mt_bytes > peak_mt) peak_mt = ks.mt_bytes;
                if (ks.l0_bytes > peak_l0) peak_l0 = ks.l0_bytes;
            }
        }
    }
    for (int t = 0; t < 20; t++) tick_all(n);
    struct efs_raft_log_progress lp2;
    struct efs_raft_peer_progress pp;
    CHECK(efs_raft_log_progress(n->r[2], &lp2) == EFS_OK, "follower progress");
    CHECK(efs_raft_peer_progress(n->r[0], 2, &pp) == EFS_OK, "leader view of 2");
    fprintf(stderr, "  stalled: commit=%llu f2.applied=%llu f2.last=%llu "
            "peak_unapplied=%llu cap=%llu busy_sent=%llu leader.peer2.busy=%llu "
            "hold=%d refused=%d peak_mt=%llu peak_l0=%llu\n",
            (unsigned long long)efs_raft_commit(n->r[0]),
            (unsigned long long)efs_raft_applied(n->r[2]),
            (unsigned long long)lp2.last_index,
            (unsigned long long)peak_unapplied, (unsigned long long)cap,
            (unsigned long long)lp2.busy_sent, (unsigned long long)pp.busy,
            pp.busy_hold, refused, (unsigned long long)peak_mt,
            (unsigned long long)peak_l0);
    CHECK(refused == 0, "healthy leader admitted every proposal");
    CHECK(efs_raft_commit(n->r[0]) >= (uint64_t)total, "quorum kept committing");
    CHECK(efs_raft_applied(n->r[1]) >= (uint64_t)total, "healthy follower applied");
    CHECK(pp.busy > 0, "leader recorded the busy peer (advisory or refusal)");
    CHECK(pp.busy_hold, "leader withholds entries from the busy peer");
    {
        /* A leader that ignores the advisory (e.g. a new leader with no
         * busy state) sends a full batch: the follower must refuse it busy,
         * append nothing and hint its proven prefix. */
        struct efs_raft_msg ae;
        static uint8_t cmds[EFS_RAFT_AE_MAX][32];
        uint64_t before = lp2.last_index, rejects0 = n->busy_rejects_seen;
        memset(&ae, 0, sizeof(ae));
        ae.type = EFS_RAFT_MSG_AE_REQ;
        ae.from = 0;
        ae.to = 2;
        ae.group = 0;
        ae.term = efs_raft_term(n->r[0]);
        ae.boot_id = 1;
        ae.prev_index = lp2.last_index;
        ae.prev_term = lp2.last_term;
        ae.leader_commit = efs_raft_commit(n->r[0]);
        ae.nentries = EFS_RAFT_AE_MAX;
        for (uint32_t i = 0; i < EFS_RAFT_AE_MAX; i++) {
            memset(cmds[i], 0x7b, sizeof(cmds[i]));
            ae.entries[i].term = ae.term;
            ae.entries[i].cmd = cmds[i];
            ae.entries[i].clen = sizeof(cmds[i]);
        }
        CHECK(efs_raft_recv(n->r[2], &ae) == EFS_OK, "follower handled AE");
        CHECK(n->busy_rejects_seen == rejects0 + 1, "full batch refused busy");
        CHECK(efs_raft_log_progress(n->r[2], &lp2) == EFS_OK &&
              lp2.last_index == before, "refused batch appended nothing");
        CHECK(lp2.busy_sent > 0, "follower counted the busy refusal");
        CHECK(unapplied(n->r[2]) <= cap, "still within the cap");
        deliver(n); /* the leader takes the busy refusal like any other */
    }
    CHECK(peak_unapplied <= cap, "follower unapplied log stayed within the cap");
    CHECK(lp2.last_index < (uint64_t)total, "follower log stopped growing (bounded)");
    if (kv) {
        CHECK(peak_mt <= kv_mem_bound, "follower memtable stayed at the budget");
        CHECK(peak_l0 <= kv_l0_bound, "follower L0 stayed at its cap");
    }

    release();
    uint64_t want = efs_raft_commit(n->r[0]);
    for (int t = 0; t < 200000 && efs_raft_applied(n->r[2]) < want; t++) {
        tick_all(n);
        if (kv && t % 16 == 0) usleep(1000); /* let the compactor run */
    }
    fprintf(stderr, "  released: f2.applied=%llu want=%llu\n",
            (unsigned long long)efs_raft_applied(n->r[2]),
            (unsigned long long)want);
    CHECK(efs_raft_applied(n->r[2]) == want, "follower caught up after release");
    CHECK(unapplied(n->r[2]) == 0, "nothing left unapplied");
    if (kv) {
        uint8_t key[16] = {0}, val[4096];
        uint32_t vl = sizeof(val);
        uint64_t last = (uint64_t)total - 1;
        key[0] = 0x7a;
        for (int b = 0; b < 8; b++) key[8 + b] = (uint8_t)(last >> (56 - 8 * b));
        CHECK(efs_kv_get(kv, key, 16, val, &vl) == EFS_OK && vl == vlen &&
              val[0] == (uint8_t)(last & 0xff), "last write applied to the KV");
    }
}

static void sim_release(void) { apps[2].park = 0; }

static void test_sim(void)
{
    struct net n;
    uint64_t cap = efs_raft_unapplied_cap(0); /* floor: two full batches */
    fprintf(stderr, "test_sim (parked apply), cap=%llu\n", (unsigned long long)cap);
    boot(&n, cap);
    apps[2].park = 1;
    drive(&n, cap, 4000, 64, sim_release, NULL, 0, 0);
    shutdown_net(&n);
}

static void test_leader_admission(void)
{
    struct net n;
    uint64_t cap = efs_raft_unapplied_cap(0);
    int busy = 0, i;
    fprintf(stderr, "test_leader_admission, cap=%llu\n", (unsigned long long)cap);
    boot(&n, cap);
    apps[0].park = 1; /* the leader's own state machine is behind */
    for (i = 0; i < (int)cap * 2 && !busy; i++)
        busy = propose_one(&n, (uint64_t)i, 32) == EFS_ERR_BUSY;
    CHECK(busy, "leader at its cap refuses EFS_ERR_BUSY");
    CHECK(unapplied(n.r[0]) <= cap, "leader unapplied within cap");
    apps[0].park = 0;
    for (int t = 0; t < 50; t++) tick_all(&n);
    CHECK(unapplied(n.r[0]) == 0, "leader drained");
    CHECK(propose_one(&n, 999999, 32) == EFS_OK, "leader admits again");
    shutdown_net(&n);
}

static int leader_of(struct net *n)
{
    for (int i = 0; i < NR; i++)
        if (efs_raft_role(n->r[i]) == EFS_RAFT_LEADER) return i;
    return -1;
}

/* D30: leadership requires local capacity. */
static void test_capacity_leadership(void)
{
    struct net n;
    struct efs_raft_log_progress lp;
    int t, l;
    fprintf(stderr, "test_capacity_leadership\n");
    boot(&n, efs_raft_unapplied_cap(0));
    efs_raft_allow_campaign(n.r[1], 1);
    /* 1. The leader's state machine is at its watermark: it steps down
     * after four election timeouts and does not campaign; node 1 leads. */
    efs_raft_set_pressure(n.r[0], 1);
    for (t = 0; t < 400 && leader_of(&n) != 1; t++) tick_all(&n);
    CHECK(leader_of(&n) == 1, "healthy peer took leadership");
    CHECK(efs_raft_log_progress(n.r[0], &lp) == EFS_OK && lp.capacity_stepdowns == 1,
          "pressured leader stepped down once");
    CHECK(lp.low_capacity == 1, "pressured replica reports low capacity");
    CHECK(propose_one(&n, 1, 32) == EFS_ERR_NOT_PRIMARY, "old leader no longer admits");
    /* 2. Both voters that may campaign are pressured: no leader, elections
     * held (node 2 never campaigns in this fixture). */
    efs_raft_set_pressure(n.r[1], 1);
    for (t = 0; t < 400; t++) tick_all(&n);
    CHECK(leader_of(&n) < 0, "no low-capacity replica leads");
    CHECK(efs_raft_log_progress(n.r[0], &lp) == EFS_OK && lp.campaigns_held > 0,
          "pressured follower held its election");
    /* 3. Pressure clears: leadership and admission return. */
    efs_raft_set_pressure(n.r[0], 0);
    efs_raft_set_pressure(n.r[1], 0);
    for (t = 0; t < 400 && leader_of(&n) < 0; t++) tick_all(&n);
    l = leader_of(&n);
    CHECK(l >= 0, "leader elected after pressure cleared");
    if (l >= 0) {
        uint64_t idx = 0;
        uint8_t cmd[8] = {0x7a};
        CHECK(efs_raft_propose(n.r[l], cmd, sizeof(cmd), &idx) == EFS_OK, "admits again");
        deliver(&n);
    }
    shutdown_net(&n);
}


/* The host's admission watermark (host_admit): new work is refused while
 * the whole unapplied tail, committed or not, is at 3/4 of the cap. */
static int host_admission_held(struct efs_raft *r)
{
    struct efs_raft_log_progress lp;
    if (efs_raft_log_progress(r, &lp) != EFS_OK) return 1;
    return lp.max_unapplied && lp.unapplied >= lp.max_unapplied / 4u * 3u;
}

#define TAIL_LEN 96
#define TAIL_CAP 128

static int counted_sends;
static int count_send(void *p, const struct efs_raft_msg *m)
{
    (void)p; (void)m;
    counted_sends++;
    return EFS_OK;
}

/* Three voters restarted with identical term-1 tails of TAIL_LEN entries,
 * none known committed (commit = applied = 0), cap TAIL_CAP, no pressure,
 * all campaign-enabled. send = NULL delivers through the queued net. */
static void boot_tail(struct net *n, efs_raft_send_fn send)
{
    struct efs_raft_cfg cfg;
    memset(n, 0, sizeof(*n));
    memset(apps, 0, sizeof(apps));
    for (int i = 0; i < NR; i++) {
        uint8_t c = 0x7a;
        st[i] = efs_raft_mem_create();
        CHECK(st[i]->save_hard(st[i], 1, -1) == EFS_OK, "save hard state");
        for (uint64_t j = 1; j <= TAIL_LEN; j++)
            CHECK(st[i]->append(st[i], j, 1, &c, 1) == EFS_OK, "preload tail");
        memset(&cfg, 0, sizeof(cfg));
        cfg.id = i;
        cfg.n = NR;
        cfg.election_ticks = 10;
        cfg.heartbeat_ticks = 2;
        cfg.boot_id = 1;
        cfg.rng_seed = 7 + (uint64_t)i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.send = send ? send : q_send;
        cfg.net = n;
        cfg.apply = app_apply;
        cfg.app = &apps[i];
        n->r[i] = efs_raft_new(&cfg);
        CHECK(n->r[i] != NULL, "raft new");
        if (n->r[i]) efs_raft_set_max_unapplied(n->r[i], TAIL_CAP);
    }
}

/* S1 reproducer (formerly scripts/reviews/k22_capacity_tail.c): the send
 * callback only counts. Before the fix, 1,000 ticks sent nothing and every
 * replica held its campaigns. */
static void test_tail_campaigns(void)
{
    struct net n;
    struct efs_raft_log_progress lp;
    fprintf(stderr, "test_tail_campaigns (S1 reproducer, counting only)\n");
    counted_sends = 0;
    boot_tail(&n, count_send);
    for (int t = 0; t < 1000; t++)
        for (int i = 0; i < NR; i++) (void)efs_raft_tick(n.r[i]);
    for (int i = 0; i < NR; i++) {
        CHECK(efs_raft_log_progress(n.r[i], &lp) == EFS_OK, "progress");
        fprintf(stderr, "  node=%d role=%d last=%llu unapplied=%llu low=%llu held=%llu\n",
                i, efs_raft_role(n.r[i]), (unsigned long long)lp.last_index,
                (unsigned long long)lp.unapplied, (unsigned long long)lp.low_capacity,
                (unsigned long long)lp.campaigns_held);
        CHECK(lp.unapplied == TAIL_LEN, "tail retained, nothing applied");
        CHECK(lp.low_capacity == 0, "an uncommitted tail is not low capacity");
        CHECK(lp.campaigns_held == 0, "no campaign held for an uncommitted tail");
        CHECK(efs_raft_applied(n.r[i]) == 0, "uncommitted entries not applied");
    }
    fprintf(stderr, "  messages sent=%d\n", counted_sends);
    CHECK(counted_sends > 0, "replicas with uncommitted tails send pre-votes");
    shutdown_net(&n);
}

/* S1 recovery with delivery: a leader is elected from the tails, holds
 * admission and does not step down while the tail is uncommitted, then
 * commits and applies it and admits again. */
static void test_tail_recovery(void)
{
    struct net n;
    struct efs_raft_log_progress lp;
    int l = -1, t;
    fprintf(stderr, "test_tail_recovery (S1, delivering)\n");
    boot_tail(&n, NULL);
    /* Phase 1: AppendEntries replies are lost, so the leader cannot
     * commit anything; votes and heartbeats still flow. */
    n.drop_ae_rep = 1;
    for (t = 0; t < 400 && (l = leader_of(&n)) < 0; t++) tick_all(&n);
    CHECK(l >= 0, "a leader is elected from uncommitted tails");
    if (l < 0) { shutdown_net(&n); return; }
    uint64_t term = efs_raft_term(n.r[l]);
    for (t = 0; t < 100; t++) tick_all(&n); /* > 4 election timeouts */
    CHECK(leader_of(&n) == l && efs_raft_term(n.r[l]) == term,
          "leader with only an uncommitted tail keeps leading");
    CHECK(efs_raft_log_progress(n.r[l], &lp) == EFS_OK, "leader progress");
    CHECK(lp.capacity_stepdowns == 0, "no capacity step-down for the own tail");
    CHECK(lp.low_capacity == 0, "uncommitted tail is not low capacity");
    CHECK(lp.last_index == TAIL_LEN + 1, "leader appended only its no-op");
    CHECK(efs_raft_commit(n.r[l]) == 0, "nothing committed without acks");
    CHECK(host_admission_held(n.r[l]), "admission held while the tail is uncommitted");
    for (int i = 0; i < NR; i++)
        CHECK(efs_raft_applied(n.r[i]) == 0, "no uncommitted entry applied");
    /* Phase 2: replies flow; the no-op commits the tail everywhere. */
    n.drop_ae_rep = 0;
    for (t = 0; t < 200; t++) {
        int done = 1;
        tick_all(&n);
        for (int i = 0; i < NR; i++)
            if (efs_raft_applied(n.r[i]) < TAIL_LEN + 1) done = 0;
        if (done) break;
    }
    CHECK(leader_of(&n) == l, "same leader after commit");
    CHECK(efs_raft_commit(n.r[l]) == TAIL_LEN + 1, "tail and no-op committed");
    for (int i = 0; i < NR; i++) {
        fprintf(stderr, "  node=%d role=%d commit=%llu applied=%llu\n", i,
                efs_raft_role(n.r[i]), (unsigned long long)efs_raft_commit(n.r[i]),
                (unsigned long long)efs_raft_applied(n.r[i]));
        CHECK(efs_raft_applied(n.r[i]) == TAIL_LEN + 1, "tail applied on every replica");
        CHECK(unapplied(n.r[i]) == 0, "nothing left unapplied");
    }
    CHECK(!host_admission_held(n.r[l]), "admission watermark released");
    {
        uint64_t idx = 0;
        uint8_t cmd[8] = {0x7a};
        CHECK(efs_raft_propose(n.r[l], cmd, sizeof(cmd), &idx) == EFS_OK,
              "leader admits new work");
        for (t = 0; t < 20; t++) tick_all(&n);
        for (int i = 0; i < NR; i++)
            CHECK(efs_raft_applied(n.r[i]) >= idx, "new work applied everywhere");
    }
    shutdown_net(&n);
}

/* A committed but unapplied backlog at 3/4 of the cap still holds
 * campaigning, also after the leader is gone; once the state machine can
 * take it, the replica applies it locally and campaigns. */
static void test_committed_backlog_holds(void)
{
    struct net n;
    struct efs_raft_log_progress lp;
    /* The busy advisory parks a follower one full batch below the cap;
     * at eight batches that is past the 3/4 watermark. */
    uint64_t cap = 8u * EFS_RAFT_AE_MAX;
    int t;
    fprintf(stderr, "test_committed_backlog_holds, cap=%llu\n", (unsigned long long)cap);
    boot(&n, cap);
    apps[1].park = 1;
    for (int i = 0; i < (int)cap * 2; i++) {
        (void)propose_one(&n, (uint64_t)i, 32);
        if (i % 8 == 0) tick_all(&n);
    }
    for (t = 0; t < 60; t++) tick_all(&n);
    CHECK(efs_raft_log_progress(n.r[1], &lp) == EFS_OK, "progress");
    fprintf(stderr, "  f1: commit=%llu applied=%llu last=%llu low=%llu\n",
            (unsigned long long)efs_raft_commit(n.r[1]),
            (unsigned long long)efs_raft_applied(n.r[1]),
            (unsigned long long)lp.last_index, (unsigned long long)lp.low_capacity);
    CHECK(efs_raft_commit(n.r[1]) - efs_raft_applied(n.r[1]) >= cap / 4u * 3u,
          "follower holds a committed backlog at 3/4 of the cap");
    CHECK(lp.low_capacity == 1, "committed backlog is low capacity");
    /* The leader is lost; replica 1 may campaign, replica 2 only votes. */
    uint64_t term = efs_raft_term(n.r[1]);
    n.down = 1u << 0;
    efs_raft_allow_campaign(n.r[1], 1);
    for (t = 0; t < 200; t++) tick_all(&n);
    CHECK(efs_raft_log_progress(n.r[1], &lp) == EFS_OK && lp.campaigns_held > 0,
          "replica with a committed backlog held its campaigns");
    CHECK(efs_raft_term(n.r[1]) == term && efs_raft_role(n.r[1]) != EFS_RAFT_LEADER,
          "held replica did not start an election");
    /* The state machine recovers: no leader is needed to drain, and the
     * replica stops holding. Its log is shorter than replica 2's, so it
     * cannot win; once replica 2 may campaign too, a leader emerges. */
    apps[1].park = 0;
    for (t = 0; t < 4 * 20; t++) tick_all(&n);
    CHECK(efs_raft_applied(n.r[1]) == efs_raft_commit(n.r[1]),
          "committed backlog applied without a leader");
    CHECK(efs_raft_log_progress(n.r[1], &lp) == EFS_OK && lp.low_capacity == 0,
          "capacity regained");
    uint64_t held = lp.campaigns_held;
    for (t = 0; t < 100; t++) tick_all(&n);
    CHECK(efs_raft_log_progress(n.r[1], &lp) == EFS_OK && lp.campaigns_held == held,
          "drained replica campaigns without holding");
    efs_raft_allow_campaign(n.r[2], 1);
    /* Replica 0 is cut off and still believes it leads; look at 2. */
    for (t = 0; t < 400 && efs_raft_role(n.r[2]) != EFS_RAFT_LEADER; t++) tick_all(&n);
    CHECK(efs_raft_role(n.r[2]) == EFS_RAFT_LEADER, "the up-to-date voter leads");
    n.down = 0;
    shutdown_net(&n);
}

#if EFS_FAULTS
static char fault_path[256];
static void stall_release(void) { unlink(fault_path); }

static void test_stalled_compactor(void)
{
    struct net n;
    char dir[EFS_TEST_PATH];efs_test_path(dir,sizeof(dir),"efs-k22-XXXXXX");
    struct efs_kv_lsm_cfg kc;
    struct efs_kv_lsm_stats ks;
    FILE *f;

    CHECK(efs_test_mkdtemp(dir) != NULL, "tmpdir");
    snprintf(fault_path, sizeof(fault_path), "%s/fault", dir);
    f = fopen(fault_path, "w");
    CHECK(f != NULL, "fault file");
    if (f) fclose(f);
    setenv("EFS_FAULT_COMPACT_STALL", "1", 1);
    setenv("EFS_FAULT_COMPACT_FILE", fault_path, 1);
    memset(&kc, 0, sizeof(kc));
    kc.sync_mode = EFS_KV_LSM_NOSYNC;
    kc.memtable_max = 64u << 10;
    kc.l0_bytes = 1u << 20;
    char kvdir[300];
    snprintf(kvdir, sizeof(kvdir), "%s/kv", dir);
    struct efs_kv *kv = efs_kv_lsm_open(kvdir, &kc);
    CHECK(kv != NULL, "kv open");
    if (!kv) return;
    CHECK(efs_kv_lsm_stats(kv, &ks) == EFS_OK, "kv stats");
    uint64_t cap = efs_raft_unapplied_cap(ks.mem_budget);
    fprintf(stderr, "test_stalled_compactor: l0_cap=%llu mem_budget=%llu cap=%llu\n",
            (unsigned long long)ks.l0_cap, (unsigned long long)ks.mem_budget,
            (unsigned long long)cap);
    CHECK(ks.l0_cap == (1u << 20), "configured L0 cap");
    CHECK(ks.mem_budget == efs_kv_lsm_mem_budget(1u << 20, 64u << 10), "derived budget");
    boot(&n, cap);
    apps[2].kv = kv;
    /* 6000 x 1 KiB: ~6 MiB, far beyond L0 cap + budget + cap entries. */
    drive(&n, cap, 6000, 1024, stall_release, kv,
          ks.mem_budget + 4096, ks.l0_cap + ks.mem_budget + (64u << 10));
    CHECK(efs_kv_lsm_stats(kv, &ks) == EFS_OK, "kv stats after");
    fprintf(stderr, "  after: mt=%llu l0=%llu deferred=%llu\n",
            (unsigned long long)ks.mt_bytes, (unsigned long long)ks.l0_bytes,
            (unsigned long long)ks.apply_deferred);
    CHECK(ks.apply_deferred > 0, "apply was deferred at the budget");
    shutdown_net(&n);
    efs_kv_lsm_close(kv);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup %s failed\n", dir);
}
#endif

int main(void)
{
    test_sim();
    test_leader_admission();
    test_capacity_leadership();
    test_tail_campaigns();
    test_tail_recovery();
    test_committed_backlog_holds();
#if EFS_FAULTS
    test_stalled_compactor();
#else
    fprintf(stderr, "test_stalled_compactor: NOTRUN (needs make EFS_FAULTS=1)\n");
#endif
    if (failures) {
        fprintf(stderr, "test_follower_bound: %d FAILED\n", failures);
        return 1;
    }
    printf("test_follower_bound: OK\n");
    return 0;
}
