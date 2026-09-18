/* Isolated single-shard Raft SM. No sockets. */
#include "efs/raft.h"
#include "efs/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

struct net {
    struct efs_raft *r[EFS_RAFT_MAX_PEERS];
    int n;
    int drop[EFS_RAFT_MAX_PEERS];
};

struct app {
    int n;
    uint8_t last;
    uint64_t snap_at;
};

static void wr32_t(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32_t(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int snap_get(void *app, uint64_t last_index, uint8_t **data, uint32_t *len)
{
    struct app *a = app;
    uint8_t *p = malloc(5);

    if (!p)
        return EFS_ERR_NOMEM;
    p[0] = a->last;
    wr32_t(p + 1, (uint32_t)a->n);
    *data = p;
    *len = 5;
    a->snap_at = last_index;
    return EFS_OK;
}

static int snap_put(void *app, uint64_t last_index, const uint8_t *data,
                    uint32_t len)
{
    struct app *a = app;

    if (len < 5 || !data)
        return EFS_ERR_INVAL;
    a->last = data[0];
    a->n = (int)rd32_t(data + 1);
    a->snap_at = last_index;
    return EFS_OK;
}

static int send_now(void *net, const struct efs_raft_msg *msg)
{
    struct net *n = net;

    if (n->drop[msg->to])
        return EFS_OK;
    if (msg->to < 0 || msg->to >= n->n || !n->r[msg->to])
        return EFS_ERR_INVAL;
    /* Synchronous delivery: the sender's entry buffers stay valid for the
     * whole recv and recv copies each entry into the log store, so no
     * detach copy is needed (this also covers batched multi-entry AEs). */
    return efs_raft_recv(n->r[msg->to], msg);
}

static int apply_cmd(void *app, uint64_t index, uint64_t term, const uint8_t *cmd,
                     uint32_t clen)
{
    struct app *a = app;
    (void)index;
    (void)term;
    a->n++;
    if (clen)
        a->last = cmd[0];
    return EFS_OK;
}

static int elect(struct net *n, int ticks)
{
    int t, i, leaders;

    for (t = 0; t < ticks; t++) {
        for (i = 0; i < n->n; i++) {
            if (!n->r[i] || n->drop[i])
                continue;
            if (efs_raft_tick(n->r[i]) != EFS_OK)
                return -1;
        }
    }
    leaders = 0;
    for (i = 0; i < n->n; i++) {
        if (!n->r[i] || n->drop[i])
            continue;
        if (efs_raft_role(n->r[i]) == EFS_RAFT_LEADER)
            leaders++;
    }
    return leaders;
}

static int leader_id(struct net *n)
{
    int i;
    for (i = 0; i < n->n; i++) {
        if (!n->r[i] || n->drop[i])
            continue;
        if (efs_raft_role(n->r[i]) == EFS_RAFT_LEADER)
            return i;
    }
    return -1;
}

static void test_election_i1(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    int i, leaders;
    uint64_t term = 0;

    memset(app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    cfg.n = 3;
    cfg.send = send_now;
    cfg.net = &n;
    cfg.apply = apply_cmd;
    for (i = 0; i < 3; i++) {
        st[i] = efs_raft_mem_create();
        CHECK(st[i] != NULL, "store");
        cfg.id = i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.election_ticks = (uint32_t)(4 + i * 4);
        cfg.heartbeat_ticks = 1;
        cfg.app = &app[i];
        n.r[i] = efs_raft_new(&cfg);
        CHECK(n.r[i] != NULL, "raft");
    }
    leaders = elect(&n, 20);
    CHECK(leaders == 1, "I1 one leader");
    i = leader_id(&n);
    CHECK(i >= 0, "leader id");
    term = efs_raft_term(n.r[i]);
    CHECK(term >= 1, "term");
    for (i = 0; i < 3; i++) {
        efs_raft_free(n.r[i]);
        efs_raft_mem_free(st[i]);
    }
}

static void test_replicate_and_readindex(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    int i, lid;
    uint8_t cmd = 'A';
    uint64_t idx = 0;

    memset(app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    cfg.n = 3;
    cfg.send = send_now;
    cfg.net = &n;
    cfg.apply = apply_cmd;
    for (i = 0; i < 3; i++) {
        st[i] = efs_raft_mem_create();
        cfg.id = i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.election_ticks = (uint32_t)(4 + i * 4);
        cfg.heartbeat_ticks = 1;
        cfg.app = &app[i];
        n.r[i] = efs_raft_new(&cfg);
    }
    CHECK(elect(&n, 20) == 1, "elect");
    lid = leader_id(&n);
    CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK && idx > 0, "propose");
    elect(&n, 8); /* heartbeats + catch-up */
    CHECK(efs_raft_commit(n.r[lid]) >= idx, "committed");
    CHECK(app[0].n + app[1].n + app[2].n >= 3, "applied somewhere");
    CHECK(efs_raft_read_begin(n.r[lid]) == EFS_OK, "read begin");
    elect(&n, 4);
    CHECK(efs_raft_read_ready(n.r[lid]), "ReadIndex");
    CHECK(efs_raft_read_current(n.r[lid]), "ReadIndex still current");
    cmd = 'B';
    CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose 2");
    elect(&n, 8);
    CHECK(efs_raft_read_current(n.r[lid]),
          "commit quorum keeps ReadIndex current");
    for (i = 0; i < 3; i++) {
        efs_raft_free(n.r[i]);
        efs_raft_mem_free(st[i]);
    }
}

static void test_i3_i4_and_restart(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    int i, lid;
    uint8_t cmd = 'B';
    uint64_t idx = 0, term;
    struct efs_raft_msg fence;

    memset(app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    cfg.n = 3;
    cfg.send = send_now;
    cfg.net = &n;
    cfg.apply = apply_cmd;
    for (i = 0; i < 3; i++) {
        st[i] = efs_raft_mem_create();
        cfg.id = i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.election_ticks = (uint32_t)(4 + i * 4);
        cfg.heartbeat_ticks = 1;
        cfg.app = &app[i];
        n.r[i] = efs_raft_new(&cfg);
    }
    CHECK(elect(&n, 20) == 1, "elect");
    lid = leader_id(&n);
    term = efs_raft_term(n.r[lid]);

    /* I3: a higher-term vote request fences the leader. */
    memset(&fence, 0, sizeof(fence));
    fence.type = EFS_RAFT_MSG_VOTE_REQ;
    fence.from = (lid + 1) % 3;
    fence.to = lid;
    fence.term = term + 5;
    fence.last_log_index = 10;
    fence.last_log_term = term + 5;
    CHECK(efs_raft_recv(n.r[lid], &fence) == EFS_OK, "fence");
    CHECK(efs_raft_role(n.r[lid]) == EFS_RAFT_FOLLOWER, "I3 stepped down");
    CHECK(efs_raft_term(n.r[lid]) == term + 5, "term jumped");
    efs_raft_free(n.r[lid]);
    cfg.id = lid;
    cfg.store = st[lid];
    cfg.store_ctx = st[lid];
    cfg.app = &app[lid];
    n.r[lid] = efs_raft_new(&cfg);
    CHECK(n.r[lid] && efs_raft_term(n.r[lid]) == term + 5, "restart term");

    /* I4: drop follower traffic — the partitioned leader cannot
     * ack a new proposal (quorum is 2). */
    {
        uint64_t before;
        struct net *pn = &n;
        /* Re-elect on a fresh group for a live leader, then partition. */
        for (i = 0; i < 3; i++) {
            efs_raft_free(n.r[i]);
            efs_raft_mem_free(st[i]);
            st[i] = efs_raft_mem_create();
            cfg.id = i;
            cfg.n = 3;
            cfg.store = st[i];
            cfg.store_ctx = st[i];
            cfg.net = &n;
            cfg.election_ticks = (uint32_t)(4 + i * 4);
            cfg.app = &app[i];
            n.drop[i] = 0;
            n.r[i] = efs_raft_new(&cfg);
        }
        CHECK(elect(&n, 20) == 1, "re-elect");
        lid = leader_id(&n);
        before = efs_raft_commit(n.r[lid]);
        for (i = 0; i < 3; i++) {
            if (i != lid)
                pn->drop[i] = 1;
        }
        CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose part");
        for (i = 0; i < 6; i++)
            CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "leader tick");
        CHECK(efs_raft_commit(n.r[lid]) == before, "I4 no quorum commit");
    }
    for (i = 0; i < 3; i++) {
        efs_raft_free(n.r[i]);
        efs_raft_mem_free(st[i]);
    }
}

static void test_snapshot(void)
{
    struct net n = { .n = 1 };
    struct efs_raft_store *st = efs_raft_mem_create();
    struct app app = { 0 };
    struct efs_raft_cfg cfg;
    struct efs_raft *r;
    uint8_t cmd = 'S';
    uint64_t idx = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = 0;
    cfg.n = 1;
    cfg.store = st;
    cfg.store_ctx = st;
    cfg.send = send_now;
    cfg.net = &n;
    cfg.apply = apply_cmd;
    cfg.app = &app;
    cfg.election_ticks = 1;
    r = efs_raft_new(&cfg);
    n.r[0] = r;
    CHECK(r, "new");
    CHECK(efs_raft_tick(r) == EFS_OK, "tick1");
    if (efs_raft_role(r) != EFS_RAFT_LEADER)
        CHECK(efs_raft_tick(r) == EFS_OK, "tick2");
    CHECK(efs_raft_role(r) == EFS_RAFT_LEADER, "solo leader");
    CHECK(efs_raft_propose(r, &cmd, 1, &idx) == EFS_OK, "solo propose");
    CHECK(efs_raft_applied(r) >= idx, "applied");
    CHECK(efs_raft_snapshot(r) == EFS_OK, "snap");
    efs_raft_free(r);
    r = efs_raft_new(&cfg);
    CHECK(r && efs_raft_applied(r) >= 1, "reload snap idx");
    efs_raft_free(r);
    efs_raft_mem_free(st);
}

static void boot_n(struct net *n, struct efs_raft_store **st, struct app *app,
                  struct efs_raft_cfg *cfg, int npeers, uint32_t voters)
{
    int i;

    memset(n, 0, sizeof(*n));
    n->n = npeers;
    memset(app, 0, sizeof(struct app) * npeers);
    memset(cfg, 0, sizeof(*cfg));
    cfg->n = 3;
    cfg->voters = voters;
    cfg->send = send_now;
    cfg->net = n;
    cfg->apply = apply_cmd;
    cfg->boot_id = 1;
    for (i = 0; i < npeers; i++) {
        st[i] = efs_raft_mem_create();
        cfg->id = i;
        cfg->store = st[i];
        cfg->store_ctx = st[i];
        cfg->election_ticks = (uint32_t)(4 + i * 4);
        cfg->heartbeat_ticks = 1;
        cfg->app = &app[i];
        n->r[i] = efs_raft_new(cfg);
        CHECK(n->r[i] != NULL, "raft new");
    }
}

static void free_n(struct net *n, struct efs_raft_store **st, int npeers)
{
    int i;
    for (i = 0; i < npeers; i++) {
        efs_raft_free(n->r[i]);
        n->r[i] = NULL;
        efs_raft_mem_free(st[i]);
        st[i] = NULL;
    }
}

static void test_step_down_no_campaign(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    int lid;

    boot_n(&n, st, app, &cfg, 3, 0x7);
    CHECK(elect(&n, 30) == 1, "elect");
    lid = leader_id(&n);
    CHECK(lid >= 0, "leader");
    efs_raft_allow_campaign(n.r[lid], 0);
    CHECK(efs_raft_step_down(n.r[lid]) == EFS_OK, "step down");
    CHECK(efs_raft_role(n.r[lid]) == EFS_RAFT_FOLLOWER, "now follower");
    CHECK(elect(&n, 40) == 1, "other leader");
    CHECK(efs_raft_role(n.r[lid]) == EFS_RAFT_FOLLOWER, "hollow stays out");
    CHECK(leader_id(&n) != lid, "someone else");
    free_n(&n, st, 3);
}

static int wait_voters(struct net *n, uint32_t want, int ticks)
{
    int t, lid;

    for (t = 0; t < ticks; t++) {
        lid = leader_id(n);
        if (lid >= 0) {
            efs_raft_change(n->r[lid], want);
            if (efs_raft_voters(n->r[lid]) == want && !efs_raft_joint(n->r[lid]))
                return lid;
        }
        if (elect(n, 1) < 0)
            return -1;
    }
    return -1;
}

static void test_grow_3_to_5(void)
{
    struct net n;
    struct efs_raft_store *st[5];
    struct app app[5];
    struct efs_raft_cfg cfg;
    int lid;
    uint8_t cmd = 'G';
    uint64_t idx = 0;

    boot_n(&n, st, app, &cfg, 5, 0x7);
    CHECK(elect(&n, 20) == 1, "elect 3 of 5");
    lid = wait_voters(&n, 0x1f, 80);
    CHECK(lid >= 0, "grew to 5");
    CHECK(efs_raft_voters(n.r[lid]) == 0x1f, "voters 0x1f");
    CHECK(!efs_raft_joint(n.r[lid]), "cold");
    CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose 5");
    elect(&n, 8);
    CHECK(efs_raft_commit(n.r[lid]) >= idx, "commit after grow");
    CHECK(efs_raft_snapshot(n.r[3]) == EFS_OK, "snap joiner");
    efs_raft_free(n.r[3]);
    cfg.id = 3;
    cfg.store = st[3];
    cfg.store_ctx = st[3];
    cfg.app = &app[3];
    cfg.voters = 0x7;
    cfg.n = 3;
    n.r[3] = efs_raft_new(&cfg);
    CHECK(n.r[3] && efs_raft_voters(n.r[3]) == 0x1f, "cfg survives snap");
    free_n(&n, st, 5);
}

static void test_i18_joint_quorum(void)
{
    struct net n;
    struct efs_raft_store *st[5];
    struct app app[5];
    struct efs_raft_cfg cfg;
    int lid, i;
    uint64_t before;
    uint8_t cmd = 'X';
    uint64_t idx = 0;

    boot_n(&n, st, app, &cfg, 5, 0x7);
    CHECK(elect(&n, 20) == 1, "elect");
    lid = wait_voters(&n, 0x1f, 80);
    CHECK(lid >= 0, "grow first");
    n.drop[3] = 1;
    n.drop[4] = 1;
    if (elect(&n, 20) != 1) {
        CHECK(0, "re-elect among 0-2");
        free_n(&n, st, 5);
        return;
    }
    lid = leader_id(&n);
    CHECK(lid >= 0 && lid <= 2, "leader in reachable set");
    before = efs_raft_commit(n.r[lid]);
    CHECK(efs_raft_change(n.r[lid], 0x1c) == EFS_OK ||
              efs_raft_change(n.r[lid], 0x1c) == EFS_ERR_BUSY,
          "change started");
    for (i = 0; i < 8; i++)
        CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "leader tick");
    CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK ||
              efs_raft_role(n.r[lid]) != EFS_RAFT_LEADER,
          "propose or stepped");
    for (i = 0; i < 8; i++) {
        if (efs_raft_role(n.r[lid]) == EFS_RAFT_LEADER)
            efs_raft_tick(n.r[lid]);
    }
    if (efs_raft_role(n.r[lid]) == EFS_RAFT_LEADER)
        CHECK(efs_raft_commit(n.r[lid]) == before, "I18 no disjoint commit");
    n.drop[3] = 0;
    n.drop[4] = 0;
    lid = wait_voters(&n, 0x1c, 80);
    CHECK(lid >= 0, "heal completes shrink");
    if (lid >= 0)
        CHECK(efs_raft_voters(n.r[lid]) == 0x1c, "actual {2,3,4}");
    free_n(&n, st, 5);
}

static void test_stale_boot_id(void)
{
    struct net n;
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    struct efs_raft_msg stale;
    int lid;
    uint64_t term;

    boot_n(&n, st, app, &cfg, 3, 0x7);
    CHECK(elect(&n, 20) == 1, "elect");
    lid = leader_id(&n);
    term = efs_raft_term(n.r[lid]);
    efs_raft_free(n.r[1]);
    cfg.id = 1;
    cfg.store = st[1];
    cfg.store_ctx = st[1];
    cfg.app = &app[1];
    cfg.boot_id = 2;
    cfg.n = 3;
    cfg.voters = 0x7;
    n.r[1] = efs_raft_new(&cfg);
    CHECK(n.r[1], "restart boot 2");
    elect(&n, 6);
    lid = leader_id(&n);
    CHECK(lid >= 0, "leader after restart");
    memset(&stale, 0, sizeof(stale));
    stale.type = EFS_RAFT_MSG_VOTE_REQ;
    stale.from = 1;
    stale.to = lid;
    stale.term = term + 9;
    stale.boot_id = 1;
    stale.last_log_index = 10;
    stale.last_log_term = term + 9;
    CHECK(efs_raft_recv(n.r[lid], &stale) == EFS_OK, "stale recv");
    CHECK(efs_raft_role(n.r[lid]) == EFS_RAFT_LEADER, "stale incarnation dropped");
    CHECK(efs_raft_term(n.r[lid]) < term + 9, "term not fenced by stale");
    free_n(&n, st, 3);
}

/* A partitioned follower falls 200 entries behind; after it is reachable
 * again a handful of leader ticks must close the gap. That only happens
 * if send_ae batches (one-entry-per-AE would need 200 RTTs). */
static void test_ae_batch_catchup(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    int i, lid, fol, other;
    uint64_t last = 0;

    memset(app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    cfg.n = 3;
    cfg.send = send_now;
    cfg.net = &n;
    cfg.apply = apply_cmd;
    for (i = 0; i < 3; i++) {
        st[i] = efs_raft_mem_create();
        cfg.id = i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.election_ticks = (uint32_t)(4 + i * 4);
        cfg.heartbeat_ticks = 1;
        cfg.app = &app[i];
        n.r[i] = efs_raft_new(&cfg);
    }
    CHECK(elect(&n, 20) == 1, "elect");
    lid = leader_id(&n);
    CHECK(lid >= 0, "leader");
    fol = (lid + 1) % 3;
    other = (lid + 2) % 3;
    n.drop[fol] = 1;
    for (i = 0; i < 200; i++) {
        uint8_t cmd = (uint8_t)i;
        uint64_t idx = 0;
        CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose");
        last = idx;
        CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "ltick");
        CHECK(efs_raft_tick(n.r[other]) == EFS_OK, "otick");
    }
    CHECK(efs_raft_commit(n.r[lid]) >= last, "quorum committed 200");
    CHECK(efs_raft_applied(n.r[fol]) + 50 < last, "follower is behind");
    n.drop[fol] = 0;
    /* Leader heartbeats only: send_now delivers the AE batch (and the
     * success path immediately sends the next batch) without the
     * follower needing to tick. */
    for (i = 0; i < 4; i++)
        CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "catch tick");
    CHECK(efs_raft_applied(n.r[fol]) >= last, "batched AE caught up");
    CHECK(efs_raft_commit(n.r[fol]) >= last, "follower commit");
    free_n(&n, st, 3);
}

/* Leader compacts 40 entries, then grows 0x7 → 0x1f. Learners 3/4 have
 * an empty log, so AE cannot start at index 1. InstallSnapshot must
 * carry the frozen SM (snap_get at compact time). */
static void test_install_snapshot(void)
{
    struct net n;
    struct efs_raft_store *st[5];
    struct app app[5];
    struct efs_raft_cfg cfg;
    int i, lid;
    uint64_t snap_at = 0;

    boot_n(&n, st, app, &cfg, 5, 0x7);
    cfg.snap_get = snap_get;
    cfg.snap_put = snap_put;
    for (i = 0; i < 5; i++) {
        efs_raft_free(n.r[i]);
        cfg.id = i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.app = &app[i];
        cfg.election_ticks = (uint32_t)(4 + i * 4);
        cfg.heartbeat_ticks = 1;
        n.r[i] = efs_raft_new(&cfg);
        CHECK(n.r[i] != NULL, "raft with snap hooks");
    }
    CHECK(elect(&n, 20) == 1, "elect");
    lid = leader_id(&n);
    CHECK(lid >= 0, "leader");
    for (i = 0; i < 40; i++) {
        uint8_t cmd = (uint8_t)('A' + (i % 26));
        uint64_t idx = 0;

        CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose");
        if (elect(&n, 2) < 0)
            CHECK(0, "elect during propose");
        lid = leader_id(&n);
        CHECK(lid >= 0, "leader still");
    }
    CHECK(efs_raft_applied(n.r[lid]) >= 40, "leader applied 40");
    CHECK(efs_raft_snapshot(n.r[lid]) == EFS_OK, "leader snap");
    snap_at = app[lid].snap_at;
    CHECK(snap_at >= 40, "snap captured SM");
    lid = wait_voters(&n, 0x1f, 80);
    CHECK(lid >= 0, "grew via InstallSnapshot");
    CHECK(app[3].snap_at == snap_at, "learner 3 installed snap");
    CHECK(app[4].snap_at == snap_at, "learner 4 installed snap");
    CHECK(efs_raft_applied(n.r[3]) >= snap_at, "learner 3 applied");
    CHECK(efs_raft_applied(n.r[4]) >= snap_at, "learner 4 applied");
    CHECK(app[3].n >= 40 && app[3].last == app[lid].last, "learner 3 SM");
    free_n(&n, st, 5);
}

/* Compacting without a snap_get blob must not skip a learner ahead.
 * wait_voters stays BUSY — match never reaches commit. */
static void test_install_snapshot_needs_blob(void)
{
    struct net n;
    struct efs_raft_store *st[5];
    struct app app[5];
    struct efs_raft_cfg cfg;
    int i, lid;

    boot_n(&n, st, app, &cfg, 5, 0x7);
    CHECK(elect(&n, 20) == 1, "elect");
    lid = leader_id(&n);
    CHECK(lid >= 0, "leader");
    for (i = 0; i < 20; i++) {
        uint8_t cmd = (uint8_t)i;
        uint64_t idx = 0;

        CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose");
        if (elect(&n, 2) < 0)
            CHECK(0, "elect during propose");
        lid = leader_id(&n);
        CHECK(lid >= 0, "leader still");
    }
    CHECK(efs_raft_snapshot(n.r[lid]) == EFS_OK, "metadata-only snap");
    CHECK(wait_voters(&n, 0x1f, 40) < 0, "empty snap cannot skip apply");
    CHECK(efs_raft_applied(n.r[3]) == 0, "learner 3 still empty");
    CHECK(app[3].snap_at == 0, "learner 3 no snap_put");
    free_n(&n, st, 5);
}

int main(void)
{
    test_election_i1();
    test_step_down_no_campaign();
    test_replicate_and_readindex();
    test_i3_i4_and_restart();
    test_snapshot();
    test_grow_3_to_5();
    test_i18_joint_quorum();
    test_stale_boot_id();
    test_ae_batch_catchup();
    test_install_snapshot();
    test_install_snapshot_needs_blob();
    if (failures) {
        fprintf(stderr, "test_raft: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_raft: OK\n");
    return 0;
}
