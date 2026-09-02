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
};

static int send_now(void *net, const struct efs_raft_msg *msg)
{
    struct net *n = net;
    uint8_t copy[256];
    struct efs_raft_msg m = *msg;

    if (n->drop[msg->to])
        return EFS_OK;
    if (msg->to < 0 || msg->to >= n->n || !n->r[msg->to])
        return EFS_ERR_INVAL;
    if (m.nentries && m.entries[0].clen) {
        if (m.entries[0].clen > sizeof(copy))
            return EFS_ERR_NOMEM;
        memcpy(copy, m.entries[0].cmd, m.entries[0].clen);
        m.entries[0].cmd = copy;
    }
    return efs_raft_recv(n->r[msg->to], &m);
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
            if (efs_raft_tick(n->r[i]) != EFS_OK)
                return -1;
        }
    }
    leaders = 0;
    for (i = 0; i < n->n; i++) {
        if (efs_raft_role(n->r[i]) == EFS_RAFT_LEADER)
            leaders++;
    }
    return leaders;
}

static int leader_id(struct net *n)
{
    int i;
    for (i = 0; i < n->n; i++) {
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

int main(void)
{
    test_election_i1();
    test_replicate_and_readindex();
    test_i3_i4_and_restart();
    test_snapshot();
    if (failures) {
        fprintf(stderr, "test_raft: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_raft: OK\n");
    return 0;
}
