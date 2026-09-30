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
    int batches[EFS_RAFT_MAX_PEERS]; /* AE_REQ with entries delivered/dropped per peer */
    struct efs_raft_msg *keep;       /* deep copy of the last AE_REQ to keep_to */
    int keep_to;
    int keep_on;
    int snap_chunks;
    int snap_hold;
    int snap_hold_to;
    int snap_hold_left;
    uint64_t snap_held_off;
    int snap_restart_watch;
    uint64_t snap_restart_off;
    int snap_restart_seen;
    int cap_rep;          /* record AE_REP match_index */
    int cap_n;
    int cap_ok;
    uint64_t cap_match;
};

static void msg_free_deep(struct efs_raft_msg *m)
{
    uint32_t i;

    if (!m)
        return;
    for (i = 0; i < m->nentries; i++)
        free((void *)m->entries[i].cmd);
    free(m);
}

static struct efs_raft_msg *msg_copy_deep(const struct efs_raft_msg *src)
{
    struct efs_raft_msg *m = malloc(sizeof(*m));
    uint32_t i;

    if (!m)
        return NULL;
    *m = *src;
    for (i = 0; i < m->nentries; i++) {
        uint8_t *c = malloc(src->entries[i].clen ? src->entries[i].clen : 1);
        if (c && src->entries[i].clen)
            memcpy(c, src->entries[i].cmd, src->entries[i].clen);
        m->entries[i].cmd = c;
    }
    return m;
}

struct app {
    int n;
    uint8_t last;
    uint64_t snap_at;
    int wide; /* 40-byte snapshot so a small chunk size spans ≥ 3 chunks */
    uint8_t *part;
    uint32_t part_len;
    uint32_t part_resets;
    int part_applied;
    int drop_blob; /* next snap_open fails once; the retry re-exports */
};

struct snap_mem {
    uint8_t *p;
    uint32_t len;
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

static int snap_open(void *app_, uint64_t incl, void **handle, uint64_t *total)
{
    struct app *a = app_;
    struct snap_mem *b;
    uint32_t len = a->wide ? 40u : 5u;

    if (a->drop_blob) {
        a->drop_blob = 0;
        return EFS_ERR_NOT_FOUND;
    }
    b = calloc(1, sizeof(*b));
    if (!b)
        return EFS_ERR_NOMEM;
    b->p = calloc(1, len);
    if (!b->p) {
        free(b);
        return EFS_ERR_NOMEM;
    }
    b->len = len;
    b->p[0] = a->last;
    wr32_t(b->p + 1, (uint32_t)a->n);
    *handle = b;
    *total = len;
    a->snap_at = incl;
    return EFS_OK;
}

static int snap_read(void *handle, uint64_t offset, uint8_t *buf, uint32_t n,
                     uint32_t *got)
{
    struct snap_mem *b = handle;

    if (!b || offset > b->len)
        return EFS_ERR_INVAL;
    if (offset + n > b->len)
        n = b->len - (uint32_t)offset;
    if (n)
        memcpy(buf, b->p + offset, n);
    *got = n;
    return EFS_OK;
}

static void snap_close(void *handle)
{
    struct snap_mem *b = handle;

    if (!b)
        return;
    free(b->p);
    free(b);
}

static int snap_chunk(void *app_, uint64_t incl, uint64_t incl_term,
                      uint64_t offset, const uint8_t *data, uint32_t len,
                      int done)
{
    struct app *a = app_;
    uint8_t *p;

    (void)incl_term;
    if (offset == 0) {
        free(a->part);
        a->part = NULL;
        a->part_len = 0;
        a->part_resets++;
    } else if (offset != a->part_len) {
        return EFS_ERR_INVAL;
    }
    if (len) {
        p = realloc(a->part, a->part_len + len);
        if (!p)
            return EFS_ERR_NOMEM;
        if (data)
            memcpy(p + a->part_len, data, len);
        a->part = p;
        a->part_len += len;
    }
    if (!done)
        return EFS_OK;
    if (!a->part || a->part_len < 5) {
        free(a->part);
        a->part = NULL;
        a->part_len = 0;
        return EFS_ERR_INVAL;
    }
    a->last = a->part[0];
    a->n = (int)rd32_t(a->part + 1);
    a->snap_at = incl;
    a->part_applied = 1;
    free(a->part);
    a->part = NULL;
    a->part_len = 0;
    return EFS_OK;
}

static int send_now(void *net, const struct efs_raft_msg *msg)
{
    struct net *n = net;

    if (msg->to >= 0 && msg->to < EFS_RAFT_MAX_PEERS &&
        msg->type == EFS_RAFT_MSG_AE_REQ && msg->nentries > 0) {
        n->batches[msg->to]++;
        if (n->keep_on && msg->to == n->keep_to) {
            msg_free_deep(n->keep);
            n->keep = msg_copy_deep(msg);
        }
    }
    if (msg->type == EFS_RAFT_MSG_AE_REP && n->cap_rep) {
        n->cap_n++;
        n->cap_ok = msg->success;
        n->cap_match = msg->match_index;
    }
    if (msg->type == EFS_RAFT_MSG_SNAP_REQ) {
        n->snap_chunks++;
        if (n->snap_restart_watch && !n->snap_restart_seen) {
            n->snap_restart_off = msg->prev_index;
            n->snap_restart_seen = 1;
        }
        if (n->snap_hold && msg->to == n->snap_hold_to) {
            if (n->snap_hold_left > 0)
                n->snap_hold_left--;
            else {
                n->snap_held_off = msg->prev_index;
                return EFS_OK;
            }
        }
    }
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
    {
        uint64_t want = efs_raft_commit(n.r[lid]);
        /* The commit quorum of 'A' doubles as a read quorum (raft.c
         * advance_commit), so the leader already covers `want`. */
        CHECK(efs_raft_read_covers(n.r[lid], want), "commit quorum covers");
        CHECK(!efs_raft_read_covers(n.r[lid], want + 1), "not a later index");
        /* Hold the followers back so a fresh round stays pending: this is
         * what host_read_index batches on — a second reader must join the
         * pending round, never read_begin again (that resets its acks). */
        for (i = 0; i < 3; i++)
            n.drop[i] = (i != lid);
        CHECK(efs_raft_read_begin(n.r[lid]) == EFS_OK, "read begin");
        CHECK(efs_raft_read_pending(n.r[lid]), "round pending after begin");
        CHECK(!efs_raft_read_ready(n.r[lid]), "not ready without a quorum");
        CHECK(!efs_raft_read_covers(n.r[lid], want), "pending round covers nothing");
        for (i = 0; i < 3; i++)
            n.drop[i] = 0;
        elect(&n, 4);
        CHECK(efs_raft_read_ready(n.r[lid]), "ReadIndex");
        CHECK(!efs_raft_read_pending(n.r[lid]), "round done");
        CHECK(efs_raft_read_covers(n.r[lid], want), "finished round covers want");
        CHECK(efs_raft_read_covers(n.r[lid], want - 1), "and anything older");
    }
    CHECK(efs_raft_read_current(n.r[lid]), "ReadIndex still current");
    cmd = 'B';
    CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose 2");
    elect(&n, 8);
    CHECK(efs_raft_read_current(n.r[lid]),
          "commit quorum keeps ReadIndex current");
    CHECK(efs_raft_read_covers(n.r[lid], idx),
          "commit quorum covers the new index too");
    CHECK(!efs_raft_read_pending(n.r[0] == n.r[lid] ? n.r[1] : n.r[0]),
          "follower never reports a pending round");
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

/* Counting wrapper around the mem store's append (follower dedupe test). */
static int (*g_orig_append)(void *, uint64_t, uint64_t, const uint8_t *, uint32_t);
static int g_append_calls;

static int counting_append(void *ctx, uint64_t index, uint64_t term,
                           const uint8_t *cmd, uint32_t clen)
{
    g_append_calls++;
    return g_orig_append(ctx, index, term, cmd, clen);
}

/* Catch-up flow control (fcstor005, Sep 19 2026). (1) Leader: while a
 * behind follower has an unanswered entry batch, proposes must not resend
 * that window — one batch per heartbeat interval, not one per propose.
 * (2) Follower: an AE whose entries it already holds (same index+term)
 * must not rewrite them to the store (that was a pwrite+fsync per entry,
 * ~30 ms of pump time per duplicated 128-entry batch). */
static void test_ae_catchup_flow_control(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct efs_raft_store wrap;
    struct app app[3];
    struct efs_raft_cfg cfg;
    int i, lid, fol, other, before, calls;
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

    /* Follower store: count appends. Rebuild fol on a wrapped store so its
     * SM calls counting_append (the SM caches the store pointer). */
    wrap = *st[fol];
    g_orig_append = wrap.append;
    wrap.append = counting_append;
    efs_raft_free(n.r[fol]);
    cfg.id = fol;
    cfg.store = &wrap;
    cfg.store_ctx = st[fol];
    cfg.election_ticks = 100; /* never campaigns during the test */
    cfg.app = &app[fol];
    n.r[fol] = efs_raft_new(&cfg);
    CHECK(n.r[fol] != NULL, "fol re-new");

    /* (1) Drop fol, propose 200 WITHOUT ticking: exactly one batch may be
     * sent to fol (the first propose's), the rest are gated on its reply. */
    n.drop[fol] = 1;
    before = n.batches[fol];
    for (i = 0; i < 200; i++) {
        uint8_t cmd = (uint8_t)i;
        uint64_t idx = 0;
        CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose");
        last = idx;
    }
    CHECK(n.batches[fol] - before <= 2, "at most one catch-up batch per window while unanswered");
    CHECK(efs_raft_commit(n.r[lid]) >= last, "quorum committed without fol");
    /* One tick ages the outstanding batch past heartbeat_ticks=1 → the
     * next send (tick heartbeat) retransmits. */
    before = n.batches[fol];
    CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "tick");
    CHECK(n.batches[fol] - before >= 1, "aged batch is retransmitted on heartbeat");

    /* Undrop: catch-up proceeds one batch per reply, record the last batch. */
    n.drop[fol] = 0;
    n.keep_on = 1;
    n.keep_to = fol;
    for (i = 0; i < 4; i++)
        CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "catch tick");
    CHECK(efs_raft_applied(n.r[fol]) >= last, "fol caught up");
    CHECK(n.keep != NULL, "recorded a batch");
    (void)other;

    /* (2) Re-deliver the recorded batch: fol already holds every entry at
     * the same term → zero store appends, reply still success. */
    if (n.keep) {
        calls = g_append_calls;
        CHECK(efs_raft_recv(n.r[fol], n.keep) == EFS_OK, "dup AE recv");
        CHECK(g_append_calls == calls, "duplicate batch rewrote nothing");
        CHECK(efs_raft_applied(n.r[fol]) >= last, "fol still caught up");
    }
    n.keep_on = 0;
    msg_free_deep(n.keep);
    n.keep = NULL;
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
    cfg.snap_open = snap_open;
    cfg.snap_read = snap_read;
    cfg.snap_close = snap_close;
    cfg.snap_chunk = snap_chunk;
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

/* Leader compacts, then its PROCESS restarts before any learner has
 * installed: the store reloads snap_idx/snap_term but the snapshot blob is
 * memory-only and is gone. A restarted leader must still serve
 * InstallSnapshot by re-exporting the app state at its current applied
 * index — otherwise a follower behind snap_idx rejects the metadata-only
 * SNAP forever and can never catch up (observed live: fcstor005 starved at
 * applied=249 after a rolling restart of the compacted leader). */
static void test_install_snapshot_restarted_leader(void)
{
    struct net n;
    struct efs_raft_store *st[5];
    struct app app[5];
    struct efs_raft_cfg cfg;
    int i, lid, t;
    uint64_t snap_at = 0;

    boot_n(&n, st, app, &cfg, 5, 0x7);
    cfg.snap_open = snap_open;
    cfg.snap_read = snap_read;
    cfg.snap_close = snap_close;
    cfg.snap_chunk = snap_chunk;
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

    /* Process restart: same store (snap_idx survives), fresh efs_raft
     * (blob gone). Win the next election with the smallest deadline. */
    efs_raft_free(n.r[lid]);
    app[lid].drop_blob = 1;
    cfg.id = lid;
    cfg.store = st[lid];
    cfg.store_ctx = st[lid];
    cfg.app = &app[lid];
    cfg.election_ticks = 2;
    cfg.heartbeat_ticks = 1;
    cfg.voters = 0x7;
    cfg.n = 3;
    n.r[lid] = efs_raft_new(&cfg);
    CHECK(n.r[lid] != NULL, "leader restarted");
    for (t = 0; t < 60 && leader_id(&n) != lid; t++)
        if (elect(&n, 1) < 0)
            break;
    CHECK(leader_id(&n) == lid, "restarted node re-elected");

    lid = wait_voters(&n, 0x1f, 80);
    CHECK(lid >= 0, "grew via InstallSnapshot from restarted leader");
    CHECK(app[3].snap_at >= snap_at, "learner 3 installed regenerated snap");
    CHECK(app[4].snap_at >= snap_at, "learner 4 installed regenerated snap");
    CHECK(efs_raft_applied(n.r[3]) >= app[3].snap_at, "learner 3 applied");
    CHECK(efs_raft_applied(n.r[4]) >= app[4].snap_at, "learner 4 applied");
    CHECK(app[3].n == app[lid].n && app[3].last == app[lid].last,
          "learner 3 SM matches");
    free_n(&n, st, 5);
}

/* A snapshot larger than one chunk. A follower whose log was truncated
 * behind the snap point catches up across ≥ 3 chunks. A leader change
 * mid-transfer restarts at offset 0. Freeing the follower after the first
 * chunk leaves the partial image unapplied. */
static void test_snap_chunks(void)
{
    struct net n;
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    int i, lid, fol, keep;
    uint32_t resets;

    boot_n(&n, st, app, &cfg, 3, 0x7);
    cfg.snap_open = snap_open;
    cfg.snap_read = snap_read;
    cfg.snap_close = snap_close;
    cfg.snap_chunk = snap_chunk;
    cfg.snap_chunk_bytes = 16;
    for (i = 0; i < 3; i++) {
        efs_raft_free(n.r[i]);
        app[i].wide = 1;
        cfg.id = i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.app = &app[i];
        cfg.election_ticks = (uint32_t)(4 + i * 4);
        cfg.heartbeat_ticks = 1;
        n.r[i] = efs_raft_new(&cfg);
        CHECK(n.r[i] != NULL, "raft with chunked snap");
    }
    CHECK(elect(&n, 20) == 1, "elect");
    lid = leader_id(&n);
    CHECK(lid >= 0, "leader");
    for (i = 0; i < 8; i++) {
        uint8_t cmd = (uint8_t)('a' + i);
        uint64_t idx = 0;

        CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK, "propose");
        if (elect(&n, 2) < 0)
            CHECK(0, "elect during propose");
        lid = leader_id(&n);
    }
    for (i = 0; i < 3; i++)
        CHECK(efs_raft_snapshot(n.r[i]) == EFS_OK, "snapshot voter");
    fol = (lid + 1) % 3;
    keep = (lid + 2) % 3;
    efs_raft_free(n.r[fol]);
    efs_raft_mem_free(st[fol]);
    st[fol] = efs_raft_mem_create();
    cfg.id = fol;
    cfg.store = st[fol];
    cfg.store_ctx = st[fol];
    cfg.app = &app[fol];
    cfg.election_ticks = 40;
    cfg.voters = 0x7;
    cfg.n = 3;
    n.r[fol] = efs_raft_new(&cfg);
    CHECK(n.r[fol] != NULL, "empty follower");
    efs_raft_allow_campaign(n.r[fol], 0);
    n.snap_chunks = 0;
    n.snap_hold = 1;
    n.snap_hold_to = fol;
    n.snap_hold_left = 1;
    CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "first chunks");
    CHECK(n.snap_chunks >= 2, "at least two chunks sent");
    CHECK(app[fol].part_applied == 0, "partial snap not applied");
    CHECK(app[fol].part_len > 0, "partial bytes staged");
    resets = app[fol].part_resets;
    CHECK(resets >= 1, "offset 0 started a part");
    n.drop[fol] = 1;
    efs_raft_free(n.r[fol]);
    n.r[fol] = NULL;
    CHECK(app[fol].part_applied == 0, "crash leaves the part unapplied");
    efs_raft_mem_free(st[fol]);
    st[fol] = efs_raft_mem_create();
    cfg.store = st[fol];
    cfg.store_ctx = st[fol];
    n.r[fol] = efs_raft_new(&cfg);
    CHECK(n.r[fol] != NULL, "follower after crash");
    efs_raft_allow_campaign(n.r[fol], 0);
    efs_raft_allow_campaign(n.r[lid], 0);
    CHECK(efs_raft_step_down(n.r[lid]) == EFS_OK, "step down mid-transfer");
    CHECK(elect(&n, 40) == 1, "new leader");
    CHECK(leader_id(&n) == keep, "survivor leads");
    lid = keep;
    n.drop[fol] = 0;
    n.snap_hold = 0;
    n.snap_chunks = 0;
    n.snap_restart_watch = 1;
    n.snap_restart_seen = 0;
    for (i = 0; i < 8; i++)
        CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "resume snap");
    CHECK(n.snap_restart_seen, "new leader sent a snapshot");
    CHECK(n.snap_restart_off == 0, "transfer restarts at offset 0");
    CHECK(n.snap_chunks >= 3, "catch-up spans at least 3 chunks");
    CHECK(app[fol].part_resets > resets, "offset 0 reopened the part");
    CHECK(app[fol].part_applied == 1, "done imports the snapshot");
    CHECK(app[fol].n == app[lid].n && app[fol].last == app[lid].last,
          "follower SM matches");
    free(app[0].part);
    free(app[1].part);
    free(app[2].part);
    free_n(&n, st, 3);
}

/* Empty AppendEntries that matches prev must not report a stale
 * uncommitted tail as replicated. */
static void test_ae_reply_match_stops_at_prev(void)
{
    struct net n = { .n = 3, .cap_rep = 1 };
    struct efs_raft_store *st;
    struct app app;
    struct efs_raft_cfg cfg;
    struct efs_raft_msg ae;
    uint8_t prefix = 'C', junk = 'S';

    memset(&app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    st = efs_raft_mem_create();
    CHECK(st && st->append(st, 1, 1, &prefix, 1) == EFS_OK, "prefix");
    CHECK(st->append(st, 2, 1, &junk, 1) == EFS_OK, "stale tail");
    CHECK(st->save_hard(st, 1, -1) == EFS_OK, "hard");
    cfg.n = 3;
    cfg.id = 1;
    cfg.store = st;
    cfg.store_ctx = st;
    cfg.send = send_now;
    cfg.net = &n;
    cfg.apply = apply_cmd;
    cfg.app = &app;
    cfg.election_ticks = 10;
    cfg.heartbeat_ticks = 2;
    n.drop[0] = 1;
    n.r[1] = efs_raft_new(&cfg);
    CHECK(n.r[1] != NULL, "follower");
    efs_raft_allow_campaign(n.r[1], 0);
    memset(&ae, 0, sizeof(ae));
    ae.type = EFS_RAFT_MSG_AE_REQ;
    ae.from = 0;
    ae.to = 1;
    ae.term = 4;
    ae.prev_index = 1;
    ae.prev_term = 1;
    ae.leader_commit = 1;
    CHECK(efs_raft_recv(n.r[1], &ae) == EFS_OK, "empty ae");
    CHECK(n.cap_n == 1, "one reply");
    CHECK(n.cap_ok == 1, "prev matched");
    CHECK(n.cap_match == 1, "match stops at prev, not the stale tail");
    CHECK(app.last != 'S', "stale command was not applied");
    efs_raft_free(n.r[1]);
    efs_raft_mem_free(st);
}

/* New leader's no-op is above the send cap armed at attach. A follower
 * that still holds a stale tail at that index must receive the no-op,
 * and the leader must not commit it off the stale reply. */
static void test_noop_over_stale_tail(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    int i, lid, stale = 1;
    uint64_t last = 0, lterm = 0, et = 0;
    uint32_t clen;
    uint8_t buf[8];
    uint8_t prefix = 'C', junk = 'S';

    memset(app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    cfg.n = 3;
    cfg.send = send_now;
    cfg.net = &n;
    cfg.apply = apply_cmd;
    cfg.heartbeat_ticks = 1;
    for (i = 0; i < 3; i++) {
        st[i] = efs_raft_mem_create();
        CHECK(st[i] && st[i]->append(st[i], 1, 1, &prefix, 1) == EFS_OK, "prefix");
        CHECK(st[i]->save_hard(st[i], 1, -1) == EFS_OK, "hard");
        cfg.id = i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.election_ticks = (uint32_t)(4 + i * 4);
        cfg.app = &app[i];
        n.r[i] = efs_raft_new(&cfg);
        CHECK(n.r[i] != NULL, "raft");
        efs_raft_arm_durable(n.r[i]);
    }
    CHECK(st[stale]->append(st[stale], 2, 1, &junk, 1) == EFS_OK, "stale tail");
    n.drop[stale] = 1;
    CHECK(elect(&n, 30) == 1, "elect without the stale node");
    lid = leader_id(&n);
    CHECK(lid >= 0 && lid != stale, "leader is not the stale node");
    CHECK(st[lid]->last(st[lid], &last, &lterm) == EFS_OK, "leader last");
    CHECK(last == 2, "leader appended the no-op");
    CHECK(lterm == efs_raft_term(n.r[lid]), "no-op is in the leader term");
    n.drop[stale] = 0;
    CHECK(elect(&n, 8) == 1, "heartbeats");
    CHECK(efs_raft_durable(n.r[lid], last) == EFS_OK, "durable");
    CHECK(elect(&n, 4) == 1, "after durable");
    clen = sizeof(buf);
    CHECK(st[stale]->get(st[stale], 2, &et, buf, &clen) == EFS_OK, "stale get");
    CHECK(et == efs_raft_term(n.r[lid]), "stale tail replaced by the no-op");
    CHECK(clen == 0, "no-op carries no command");
    CHECK(app[stale].last != 'S', "stale command was not applied");
    CHECK(efs_raft_commit(n.r[lid]) >= last, "no-op committed");
    for (i = 0; i < 3; i++) {
        uint64_t et2 = 0;
        uint32_t c2 = sizeof(buf);
        CHECK(st[i]->get(st[i], last, &et2, buf, &c2) == EFS_OK, "no-op present");
        CHECK(et2 == lterm && c2 == 0, "every log has this no-op");
        efs_raft_free(n.r[i]);
        efs_raft_mem_free(st[i]);
    }
}

/* Sep 30 2026: with the durable ceiling armed, a propose that extends
 * send_idx past the outstanding batch re-sends that window (the Sep 29
 * latency rule). When the window already hit EFS_RAFT_AE_BYTES the resend
 * carries the same entries again: 300 KiB entries × N proposes to a
 * silent follower must stay at one window per heartbeat interval, not
 * one per propose. Small entries keep the replacement behaviour. */
static void test_ae_full_window_not_resent_per_propose(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    int i, lid, fol, before;
    uint64_t idx = 0;
    static uint8_t big[300u * 1024u];

    memset(app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    cfg.n = 3;
    cfg.send = send_now;
    cfg.net = &n;
    cfg.apply = apply_cmd;
    cfg.heartbeat_ticks = 1;
    for (i = 0; i < 3; i++) {
        st[i] = efs_raft_mem_create();
        cfg.id = i;
        cfg.store = st[i];
        cfg.store_ctx = st[i];
        cfg.election_ticks = (uint32_t)(4 + i * 4);
        cfg.app = &app[i];
        n.r[i] = efs_raft_new(&cfg);
        CHECK(n.r[i] != NULL, "raft");
    }
    CHECK(elect(&n, 20) == 1, "elect");
    lid = leader_id(&n);
    CHECK(lid >= 0, "leader");
    fol = (lid + 1) % 3;
    efs_raft_arm_durable(n.r[lid]);
    memset(big, 'b', sizeof(big));
    n.drop[fol] = 1;
    /* Fill one window (3 × 300 KiB < 1 MiB, the 4th does not fit), then
     * keep proposing: each propose_local + submit + durable + flush is
     * the pump's sequence. */
    before = n.batches[fol];
    for (i = 0; i < 12; i++) {
        CHECK(efs_raft_propose_local(n.r[lid], big, sizeof(big), &idx) == EFS_OK,
              "propose_local");
        CHECK(efs_raft_submit(n.r[lid], idx) == EFS_OK, "submit");
        CHECK(efs_raft_durable(n.r[lid], idx) == EFS_OK, "durable");
        CHECK(efs_raft_flush(n.r[lid]) == EFS_OK, "flush");
    }
    /* Window 1 (entries 1..3) went out on the first flushes; while it
     * is unanswered and full, proposes 4..12 must not re-send it. Allow
     * the replacement sends that built the window up to full. */
    CHECK(n.batches[fol] - before <= 4,
          "full window is not re-sent on every propose");
    /* Small entries: the replacement rule still applies (one resend per
     * propose while the window is not full). */
    n.drop[fol] = 0;
    for (i = 0; i < 6; i++)
        CHECK(efs_raft_tick(n.r[lid]) == EFS_OK, "tick");
    CHECK(efs_raft_applied(n.r[fol]) >= idx, "fol caught up after undrop");
    for (i = 0; i < 3; i++) {
        efs_raft_free(n.r[i]);
        efs_raft_mem_free(st[i]);
    }
}

int main(void)
{
    test_election_i1();
    test_ae_full_window_not_resent_per_propose();
    test_step_down_no_campaign();
    test_replicate_and_readindex();
    test_i3_i4_and_restart();
    test_snapshot();
    test_grow_3_to_5();
    test_i18_joint_quorum();
    test_stale_boot_id();
    test_ae_reply_match_stops_at_prev();
    test_noop_over_stale_tail();
    test_ae_batch_catchup();
    test_ae_catchup_flow_control();
    test_install_snapshot();
    test_install_snapshot_needs_blob();
    test_install_snapshot_restarted_leader();
    test_snap_chunks();
    if (failures) {
        fprintf(stderr, "test_raft: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_raft: OK\n");
    return 0;
}
