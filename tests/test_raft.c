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
    int flow_zero_probe, flow_refuse_data; /* transport pressure fixture */
    int hold_ae_replies; /* deliver append bytes but delay their acknowledgements */
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
    int park_apply;
    unsigned pressure_mask;
    struct efs_raft *owner;
    uint64_t pending_limit;
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
    if (n->hold_ae_replies && msg->type == EFS_RAFT_MSG_AE_REP)
        return EFS_OK;
    if (n->drop[msg->to])
        return EFS_OK;
    if (msg->to < 0 || msg->to >= n->n || !n->r[msg->to])
        return EFS_ERR_INVAL;
    if (n->flow_refuse_data && msg->type==EFS_RAFT_MSG_AE_REQ && msg->nentries)
        return EFS_OK; /* transport refusal cannot earn an AE_REP */
    if (n->flow_zero_probe && msg->type==EFS_RAFT_MSG_AE_REQ && !msg->nentries) {
        struct efs_raft_msg probe=*msg;
        probe.prev_index=probe.prev_term=probe.last_log_term=probe.match_index=0;
        probe.vote_granted=1;
        return efs_raft_recv(n->r[probe.to],&probe);
    }
    /* Synchronous delivery: the sender's entry buffers stay valid for the
     * whole recv and recv copies each entry into the log store, so no
     * detach copy is needed (this also covers batched multi-entry AEs). */
    return efs_raft_recv(n->r[msg->to], msg);
}

static int apply_cmd(void *app, uint64_t index, uint64_t term, const uint8_t *cmd,
                     uint32_t clen)
{
    struct app *a = app;
    if (a->park_apply) return EFS_ERR_BUSY;
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
        /* A write quorum predating a request cannot authorize that read. */
        CHECK(!efs_raft_read_covers(n.r[lid], want), "write quorum is not fresh read authority");
        CHECK(!efs_raft_read_covers(n.r[lid], want + 1), "not a later index");
        /* Hold followers back and inject successful replies from an older
         * round. They must not satisfy the new request's quorum. */
        for (i = 0; i < 3; i++)
            n.drop[i] = (i != lid);
        CHECK(efs_raft_read_begin(n.r[lid]) == EFS_OK, "read begin");
        CHECK(efs_raft_read_pending(n.r[lid]), "round pending after begin");
        CHECK(!efs_raft_read_ready(n.r[lid]), "not ready without a quorum");
        {
            struct efs_raft_msg delayed = {0};
            delayed.type = EFS_RAFT_MSG_AE_REP;
            delayed.to = lid;
            delayed.term = efs_raft_term(n.r[lid]);
            delayed.success = 1;
            delayed.match_index = want;
            delayed.last_log_index = efs_raft_read_round(n.r[lid]) - 1;
            for (i = 0; i < 3; i++) {
                if (i == lid)
                    continue;
                delayed.from = i;
                CHECK(efs_raft_recv(n.r[lid], &delayed) == EFS_OK, "deliver delayed reply");
            }
            CHECK(!efs_raft_read_ready(n.r[lid]), "old replies cannot authorize new round");
        }
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
    CHECK(!efs_raft_read_current(n.r[lid]), "new commit needs another read round");
    CHECK(!efs_raft_read_covers(n.r[lid], idx), "old round cannot cover new commit");
    CHECK(efs_raft_read_begin(n.r[lid]) == EFS_OK, "fresh round after write");
    elect(&n, 4);
    CHECK(efs_raft_read_current(n.r[lid]), "fresh quorum covers new commit");
    {
        uint64_t completed = efs_raft_read_round(n.r[lid]);
        struct efs_raft_msg delayed = {0};
        for (i = 0; i < 3; i++)
            n.drop[i] = (i != lid);
        CHECK(efs_raft_read_begin(n.r[lid]) == EFS_OK, "partitioned leader begins fresh read");
        delayed.type = EFS_RAFT_MSG_AE_REP;
        delayed.to = lid;
        delayed.term = efs_raft_term(n.r[lid]);
        delayed.success = 1;
        delayed.match_index = idx;
        delayed.last_log_index = completed;
        for (i = 0; i < 3; i++) {
            if (i == lid)
                continue;
            delayed.from = i;
            CHECK(efs_raft_recv(n.r[lid], &delayed) == EFS_OK, "old completed-round reply");
        }
        CHECK(!efs_raft_read_ready(n.r[lid]), "partition cannot reuse completed authority");
        delayed.term++;
        CHECK(efs_raft_recv(n.r[lid], &delayed) == EFS_OK, "higher term fences read");
        CHECK(!efs_raft_read_done(n.r[lid]), "stepped-down leader has no read authority");
    }
    CHECK(!efs_raft_read_pending(n.r[0] == n.r[lid] ? n.r[1] : n.r[0]),
          "follower never reports a pending round");
    for (i = 0; i < 3; i++) {
        efs_raft_free(n.r[i]);
        efs_raft_mem_free(st[i]);
    }
}

/* A mapping command can be proven committed while KV apply is unavailable.
 * Such proof must never authorize a KV read or reuse a pre-request quorum. */
static void test_read_quorum_before_apply(void)
{
    struct net n={.n=3};struct app app[3]={0};struct efs_raft_store *store[3];
    struct efs_raft_cfg cfg={0};
    cfg.n=3;cfg.send=send_now;cfg.net=&n;cfg.apply=apply_cmd;cfg.heartbeat_ticks=1;
    for(int i=0;i<3;i++) {
        store[i]=efs_raft_mem_create();cfg.id=i;cfg.store=store[i];cfg.store_ctx=store[i];
        cfg.election_ticks=4+i*4;cfg.app=&app[i];n.r[i]=efs_raft_new(&cfg);
    }
    CHECK(elect(&n,20)==1,"proof fixture elects");
    int leader=leader_id(&n);uint64_t index=0;uint8_t cmd='P';
    for(int i=0;i<3;i++)app[i].park_apply=1;
    (void)efs_raft_propose(n.r[leader],&cmd,1,&index);elect(&n,4);
    CHECK(index && efs_raft_commit(n.r[leader])>=index,"mapping committed under parked apply");
    CHECK(efs_raft_applied(n.r[leader])<index,"mapping not applied");
    for(int i=0;i<3;i++)n.drop[i]=(i!=leader);
    CHECK(efs_raft_read_begin(n.r[leader])==EFS_OK,"proof read starts");
    CHECK(!efs_raft_read_quorum(n.r[leader]),"no proof from older write quorum");
    struct efs_raft_msg old={0};old.type=EFS_RAFT_MSG_AE_REP;old.to=leader;
    old.term=efs_raft_term(n.r[leader]);old.success=1;old.match_index=index;
    old.last_log_index=efs_raft_read_round(n.r[leader])-1;
    for(int i=0;i<3;i++)if(i!=leader){old.from=i;(void)efs_raft_recv(n.r[leader],&old);}
    CHECK(!efs_raft_read_quorum(n.r[leader]),"old round replies cannot prove current request");
    for(int i=0;i<3;i++)n.drop[i]=0;
    elect(&n,4);
    CHECK(efs_raft_read_quorum(n.r[leader]),"fresh quorum proof independent of apply");
    CHECK(!efs_raft_read_ready(n.r[leader]) && !efs_raft_read_covers(n.r[leader],index),
          "proof is not KV read authority");
    for(int i=0;i<3;i++)app[i].park_apply=0;
    elect(&n,4);
    CHECK(efs_raft_read_ready(n.r[leader]) && efs_raft_applied(n.r[leader])>=index,
          "release permits authoritative reads");
    old.term++;old.from=(leader+1)%3;(void)efs_raft_recv(n.r[leader],&old);
    CHECK(!efs_raft_read_quorum(n.r[leader]),"stepdown invalidates proof authority");
    for(int i=0;i<3;i++){efs_raft_free(n.r[i]);efs_raft_mem_free(store[i]);}
}

/* A fresh read must send its own context even when an older data batch
 * remains outstanding. No timer tick or extra entry batch may be required. */
static void test_readindex_outstanding_batch(void)
{
    struct net n = { .n = 3 };
    struct efs_raft_store *st[3];
    struct app app[3] = {{0}};
    struct efs_raft_cfg cfg = {0};
    uint8_t cmd = 'R';
    uint64_t idx = 0;
    int i, lid, batches[3];
    cfg.n=3;cfg.send=send_now;cfg.net=&n;cfg.apply=apply_cmd;
    cfg.heartbeat_ticks=1;
    for (i=0;i<3;i++) {
        st[i]=efs_raft_mem_create();cfg.id=i;cfg.store=st[i];
        cfg.store_ctx=st[i];cfg.app=&app[i];cfg.election_ticks=4+i*4;
        n.r[i]=efs_raft_new(&cfg);
    }
    CHECK(elect(&n,20)==1,"elect outstanding read fixture");
    lid=leader_id(&n);
    efs_raft_arm_durable(n.r[lid]);
    CHECK(efs_raft_propose_local(n.r[lid],&cmd,1,&idx)==EFS_OK,"append local");
    CHECK(efs_raft_submit(n.r[lid],idx)==EFS_OK,"submit local");
    CHECK(efs_raft_durable(n.r[lid],idx)==EFS_OK,"local durable");
    n.hold_ae_replies=1;
    CHECK(efs_raft_flush(n.r[lid])==EFS_OK,"send outstanding batch");
    for (i=0;i<3;i++) batches[i]=n.batches[i];
    n.hold_ae_replies=0;
    CHECK(efs_raft_read_begin(n.r[lid])==EFS_OK,"begin fresh round behind batch");
    CHECK(efs_raft_read_ready(n.r[lid]),"fresh quorum completes without heartbeat tick");
    for (i=0;i<3;i++) {
        CHECK(n.batches[i]==batches[i],"fresh read does not resend outstanding entries");
        efs_raft_free(n.r[i]);efs_raft_mem_free(st[i]);
    }
}

static int pressure_send_ready(void *net, int peer, uint8_t group)
{ (void)peer;(void)group;return !((struct net *)net)->flow_refuse_data; }

static void test_transport_pressure_zero_probe(void)
{
    for (int group=0;group<=2;group+=2) {
        struct net n={.n=3};struct efs_raft_store *st[3];struct app app[3]={{0}};
        struct efs_raft_cfg cfg={0};uint8_t cmd='F';uint64_t idx=0;
        cfg.n=3;cfg.group=(uint8_t)group;cfg.send=send_now;cfg.net=&n;
        cfg.send_ready=pressure_send_ready;
        cfg.apply=apply_cmd;cfg.heartbeat_ticks=1;
        for(int i=0;i<3;i++) {
            st[i]=efs_raft_mem_create();cfg.id=i;cfg.store=st[i];cfg.store_ctx=st[i];
            cfg.app=&app[i];cfg.election_ticks=4+i*4;n.r[i]=efs_raft_new(&cfg);
        }
        CHECK(elect(&n,20)==1,"elect transport-pressure fixture");
        int lid=leader_id(&n);uint64_t committed=efs_raft_commit(n.r[lid]);
        n.flow_refuse_data=n.flow_zero_probe=1;
        efs_raft_arm_durable(n.r[lid]);
        CHECK(efs_raft_propose_local(n.r[lid],&cmd,1,&idx)==EFS_OK,"pressure append local");
        CHECK(efs_raft_submit(n.r[lid],idx)==EFS_OK,"pressure submit local");
        CHECK(efs_raft_durable(n.r[lid],idx)==EFS_OK,"pressure local durable");
        int builds=0;for(int i=0;i<3;i++)builds+=n.batches[i];
        CHECK(efs_raft_flush(n.r[lid])==EFS_OK,"transport refuses data");
        int after=0;for(int i=0;i<3;i++)after+=n.batches[i];
        CHECK(builds==after,"unavailable host queue defers entry frame construction");
        CHECK(efs_raft_commit(n.r[lid])==committed && idx>committed,"transport handling cannot commit refused tail");
        CHECK(efs_raft_read_begin(n.r[lid])==EFS_OK,"pressure fresh read starts");
        CHECK(efs_raft_read_ready(n.r[lid]),"zero-prefix current-term fresh probes establish read quorum");
        CHECK(efs_raft_commit(n.r[lid])==committed,"fresh probe quorum does not replicate refused data");
        n.flow_refuse_data=n.flow_zero_probe=0;
        for(int t=0;t<5;t++)for(int i=0;i<3;i++)efs_raft_tick(n.r[i]);
        CHECK(efs_raft_commit(n.r[lid])>=idx,"Raft retries refused tail after pressure clears");
        for(int i=0;i<3;i++){CHECK(efs_raft_applied(n.r[i])>=idx,"both groups catch up refused work");efs_raft_free(n.r[i]);efs_raft_mem_free(st[i]);}
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

static void test_solo_durable_cap_then_grow(void)
{
    struct net n;
    struct efs_raft_store *st[3];
    struct app app[3];
    struct efs_raft_cfg cfg;
    struct efs_raft_msg rejected;
    uint8_t cmd = 'Q';
    uint64_t idx = 0;
    int lid;

    boot_n(&n, st, app, &cfg, 3, 0x1);
    CHECK(elect(&n, 30) == 1, "solo election");
    lid = leader_id(&n);
    CHECK(lid == 0, "configured solo leader");
    efs_raft_arm_durable(n.r[lid]);
    CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK,
          "solo synchronous append");
    CHECK(efs_raft_applied(n.r[lid]) >= idx, "solo entry committed");
    n.drop[1] = n.drop[2] = 1;
    n.keep_on = 1; n.keep_to = 1;
    CHECK(efs_raft_change(n.r[lid], 0x7) == EFS_ERR_BUSY,
          "new learners must catch up first");
    memset(&rejected, 0, sizeof(rejected));
    rejected.type = EFS_RAFT_MSG_AE_REP;
    rejected.from = 1; rejected.to = lid;
    rejected.term = efs_raft_term(n.r[lid]);
    rejected.match_index = idx - 1;
    CHECK(efs_raft_recv(n.r[lid], &rejected) == EFS_OK, "learner requests tail");
    CHECK(n.keep && n.keep->prev_index + n.keep->nentries >= idx,
          "committed solo tail is included in learner batch");
    n.keep_on = 0; msg_free_deep(n.keep); n.keep = NULL;
    free_n(&n, st, 3);
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

static int discard_send(void *net, const struct efs_raft_msg *msg)
{
    (void)net; (void)msg;
    return EFS_OK;
}
static int fail_cfg_save(void *ctx, uint32_t old, uint32_t next)
{
    (void)ctx; (void)old; (void)next;
    return EFS_ERR_IO;
}
static int fail_cfg_load(void *ctx, uint32_t *old, uint32_t *next)
{
    (void)ctx; (void)old; (void)next;
    return EFS_ERR_IO;
}
static void test_membership_persistence(void)
{
    struct efs_raft_store *st = efs_raft_mem_create();
    struct efs_raft_cfg cfg = {0};
    struct efs_raft *r;
    struct app app = {0};
    uint64_t idx = 0, snap = 0, term = 0;
    uint8_t cmd = 'M';
    int i;
    cfg.id = 0; cfg.n = 1; cfg.voters = 1; cfg.store = st;
    cfg.store_ctx = st; cfg.send = discard_send; cfg.apply = apply_cmd;
    cfg.app = &app; cfg.election_ticks = 2; cfg.heartbeat_ticks = 1;
    r = efs_raft_new(&cfg);
    CHECK(r != NULL, "membership persistence fixture");
    if (!r) { efs_raft_mem_free(st); return; }
    for (i = 0; i < 8; i++) efs_raft_tick(r);
    CHECK(efs_raft_propose(r, &cmd, 1, &idx) == EFS_OK, "solo command committed");
    st->save_cfg = fail_cfg_save;
    CHECK(efs_raft_snapshot(r) == EFS_ERR_IO, "snapshot reports membership save failure");
    CHECK(st->load_snap(st, &snap, &term) == EFS_OK && snap == 0,
          "failed membership save cannot discard log prefix");
    CHECK(efs_raft_tick(r) == EFS_ERR_IO, "failed membership save fences replica");
    CHECK(efs_raft_role(r) == EFS_RAFT_FOLLOWER, "fenced replica loses leadership");
    efs_raft_free(r); efs_raft_mem_free(st);

    st = efs_raft_mem_create(); cfg.store = st; cfg.store_ctx = st;
    CHECK(st->save_cfg(st, 0, 0) == EFS_OK, "seed invalid empty membership");
    r = efs_raft_new(&cfg);
    CHECK(r == NULL, "cold bootstrap rejects invalid stored membership");
    efs_raft_free(r);
    CHECK(st->save_cfg(st, 1, 1u << EFS_RAFT_MAX_PEERS) == EFS_OK,
          "seed invalid joint membership");
    r = efs_raft_new(&cfg);
    CHECK(r == NULL, "cold bootstrap rejects invalid stored joint membership");
    efs_raft_free(r);
    st->load_cfg = fail_cfg_load;
    r = efs_raft_new(&cfg);
    CHECK(r == NULL, "membership load IO cannot silently use bootstrap config");
    efs_raft_free(r); efs_raft_mem_free(st);

    st = efs_raft_mem_create(); cfg.store = st; cfg.store_ctx = st;
    cfg.id = 1; cfg.n = 4; cfg.voters = 0x7; cfg.snap_chunk = snap_chunk;
    r = efs_raft_new(&cfg);
    CHECK(r != NULL, "follower membership fixture");
    if (r) {
        struct efs_raft_msg m = {0};
        struct efs_raft_entry *entry = &m.entries[0];
        uint8_t payload[13] = {0};
        m.type = EFS_RAFT_MSG_SNAP_REQ; m.from = 0; m.to = 1;
        m.term = 1; m.boot_id = 1; m.last_log_index = 10;
        m.last_log_term = 1; m.leader_commit = 10; m.success = 1;
        m.nentries = 1;
        entry->cmd = payload; entry->clen = sizeof(payload); entry->term = 1;
        wr32_t(payload, 0); wr32_t(payload+4, 0);
        CHECK(efs_raft_recv(r, &m) == EFS_OK, "malformed snapshot gets rejection response");
        CHECK(app.part_resets == 0 && !app.part_applied,
              "invalid snapshot membership rejected before importing user state");
        CHECK(efs_raft_voters(r) == 0x7, "invalid snapshot preserves original membership");
        /* An ordinary committed membership entry must persist before apply
         * advancement or a successful replication response. */
        m.type = EFS_RAFT_MSG_AE_REQ; m.last_log_index = 0;
        m.last_log_term = 0; m.leader_commit = 1; m.success = 0;
        payload[0] = EFS_RAFT_CMD_JOINT;
        wr32_t(payload+1, 0x7); wr32_t(payload+5, 0xf); entry->clen = 9;
        st->save_cfg = fail_cfg_save;
        CHECK(efs_raft_recv(r, &m) == EFS_ERR_IO, "applied membership IO is propagated");
        CHECK(efs_raft_applied(r) == 0, "failed membership does not advance apply");
        CHECK(efs_raft_tick(r) == EFS_ERR_IO, "membership apply IO fences follower");
        efs_raft_free(r);
    }
    efs_raft_mem_free(st);
}

/* Four-voter engine support is staged. These memory-store checks establish
 * replication and quorum behavior, not recoverable commit certificates or
 * production D36 protection. In particular two survivors cannot elect. */
static void test_four_voters(void)
{
    int omitted, lost_a, lost_b;

    CHECK(!efs_raft_voters_valid(0), "empty membership rejected");
    CHECK(!efs_raft_voters_valid(1u << EFS_RAFT_MAX_PEERS),
          "out-of-range membership rejected");
    CHECK(!efs_raft_voters_valid(0xffffffffu), "invalid high bits rejected");
    CHECK(efs_raft_voters_valid(0xf), "four voters accepted");
    for (omitted = 0; omitted < 4; omitted++) {
        struct net n;
        struct efs_raft_store *st[4];
        struct app app[4];
        struct efs_raft_cfg cfg;
        uint8_t cmd = '4';
        uint64_t idx = 0, term = 0;
        int lid, i, copies = 0;

        boot_n(&n, st, app, &cfg, 4, 0xf);
        for (i = 0; i < 4; i++) {
            if (!n.r[i]) {
                free_n(&n, st, 4);
                return;
            }
        }
        n.drop[omitted] = 1;
        CHECK(elect(&n, 40) == 1, "three of four elect with any node absent");
        lid = leader_id(&n);
        if (lid < 0) {
            free_n(&n, st, 4);
            return;
        }
        CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK,
              "four-voter proposal");
        CHECK(elect(&n, 8) == 1, "four-voter leader stable");
        CHECK(efs_raft_commit(n.r[lid]) >= idx, "three of four commit");
        for (i = 0; i < 4; i++) {
            uint8_t got = 0;
            uint32_t len = 1;
            if (st[i]->get(st[i], idx, &term, &got, &len) == EFS_OK &&
                len == 1 && got == cmd)
                copies++;
        }
        CHECK(copies == 3, "exact command copied to three reachable stores");
        /* Independently inspect every two-loss pair: at least one command
         * copy remains, even when the unavailable replica is a survivor. */
        for (lost_a = 0; lost_a < 4; lost_a++) {
            for (lost_b = lost_a + 1; lost_b < 4; lost_b++) {
                int retained = 0;
                for (i = 0; i < 4; i++) {
                    uint8_t got = 0;
                    uint32_t len = 1;
                    if (i != lost_a && i != lost_b &&
                        st[i]->get(st[i], idx, &term, &got, &len) == EFS_OK &&
                        len == 1 && got == cmd)
                        retained++;
                }
                CHECK(retained >= 1, "every two-loss pair retains a log copy");
            }
        }
        /* A pending read and write must not earn authority from just two
         * voters. Keep the old leader reachable to test stale leadership. */
        for (i = 0; i < 4; i++) {
            if (i != lid && i != omitted) {
                n.drop[i] = 1;
                break;
            }
        }
        {
            uint64_t before = efs_raft_commit(n.r[lid]);
            uint64_t pending = 0;
            CHECK(efs_raft_read_begin(n.r[lid]) == EFS_OK,
                  "read admitted before quorum loss is detected");
            CHECK(!efs_raft_read_quorum(n.r[lid]) &&
                  !efs_raft_read_ready(n.r[lid]), "two voters cannot authorize read");
            CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &pending) == EFS_OK,
                  "isolated leader may enqueue but cannot commit");
            CHECK(elect(&n, 40) >= 0, "two-voter ticks succeed");
            CHECK(efs_raft_commit(n.r[lid]) == before,
                  "two voters cannot acknowledge a new write");
            CHECK(!efs_raft_read_quorum(n.r[lid]), "no quorum after timeout");
        }
        /* Cold restart all surviving stores with the same four-voter config:
         * no implicit quorum override or survivor promotion. */
        for (i = 0; i < 4; i++) {
            efs_raft_free(n.r[i]);
            n.r[i] = NULL;
        }
        for (i = 0; i < 4; i++) {
            if (n.drop[i])
                continue;
            cfg.id = i; cfg.store = st[i]; cfg.store_ctx = st[i];
            cfg.app = &app[i]; cfg.boot_id++;
            n.r[i] = efs_raft_new(&cfg);
            CHECK(n.r[i] != NULL, "cold four-voter restart");
        }
        CHECK(elect(&n, 60) == 0, "two cold survivors cannot elect a leader");
        free_n(&n, st, 4);
    }
}

static void test_grow_3_to_4(void)
{
    struct net n;
    struct efs_raft_store *st[4];
    struct app app[4];
    struct efs_raft_cfg cfg;
    uint8_t cmd = 'J';
    uint64_t idx = 0;
    int lid;

    boot_n(&n, st, app, &cfg, 4, 0x7);
    CHECK(elect(&n, 20) == 1, "elect before four-voter reconfiguration");
    lid = wait_voters(&n, 0xf, 80);
    CHECK(lid >= 0, "joint consensus grows three to four");
    if (lid < 0) {
        free_n(&n, st, 4);
        return;
    }
    CHECK(efs_raft_propose(n.r[lid], &cmd, 1, &idx) == EFS_OK,
          "propose after four-voter reconfiguration");
    elect(&n, 8);
    CHECK(efs_raft_commit(n.r[lid]) >= idx, "four-voter configuration commits");
    CHECK(efs_raft_snapshot(n.r[3]) == EFS_OK, "snapshot four-voter membership");
    efs_raft_free(n.r[3]);
    cfg.id = 3; cfg.store = st[3]; cfg.store_ctx = st[3]; cfg.app = &app[3];
    n.r[3] = efs_raft_new(&cfg); /* Bootstrap says three; snapshot is authoritative. */
    CHECK(n.r[3] && efs_raft_voters(n.r[3]) == 0xf,
          "cold snapshot restores four-voter membership");
    lid = wait_voters(&n, 0x7, 80);
    CHECK(lid >= 0, "joint consensus shrinks four to three");
    free_n(&n, st, 4);
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
    ae.leader_commit = 2; /* Leader committed its own replacement at index 2. */
    CHECK(efs_raft_recv(n.r[1], &ae) == EFS_OK, "empty ae");
    CHECK(n.cap_n == 1, "one reply");
    CHECK(n.cap_ok == 1, "prev matched");
    CHECK(n.cap_match == 1, "match stops at prev, not the stale tail");
    CHECK(app.last != 'S', "stale command was not applied");
    CHECK(efs_raft_commit(n.r[1])==1 && app.n==1,"commit stops at request-proven prefix");
    ae.nentries=1;ae.entries[0].term=4;ae.entries[0].cmd=(const uint8_t*)"R";ae.entries[0].clen=1;
    CHECK(efs_raft_recv(n.r[1],&ae)==EFS_OK && efs_raft_commit(n.r[1])==2 && app.last=='R',
          "matched replacement commits after stale tail truncation");
    ae.nentries=0;ae.leader_commit=3;
    CHECK(efs_raft_recv(n.r[1],&ae)==EFS_OK && efs_raft_commit(n.r[1])==2,
          "short-prefix probe never rolls commit backward");
    ae.prev_term=99;
    CHECK(efs_raft_recv(n.r[1],&ae)==EFS_ERR_PROTO,"reject conflict with committed prev without truncation");
    ae.prev_term=1;ae.nentries=1;ae.entries[0].term=99;
    CHECK(efs_raft_recv(n.r[1],&ae)==EFS_ERR_PROTO,"reject replacement of committed entry");
    uint64_t last_index=0,last_term=0;
    CHECK(st->last(st,&last_index,&last_term)==EFS_OK && last_index==2 && last_term==4,
          "committed bytes remain intact after malformed conflicts");
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


/* Durable follower request batching: callbacks must never see pending bytes. */
static struct {
    struct efs_raft_store original;
    unsigned depth, barriers, replies, applies, first_ack_applies;
    int fail_end, fail_append, fail_hard, fail_truncate, fail_apply;
    uint64_t durable, read_context;
} recv_test;
static int recv_test_truncate(void *ctx,uint64_t index)
{
    if(recv_test.fail_truncate)return EFS_ERR_IO;
    return recv_test.original.truncate_from(ctx,index);
}
static int recv_test_hard(void *ctx,uint64_t term,int32_t voted)
{
    if(recv_test.fail_hard)return EFS_ERR_IO;
    return recv_test.original.save_hard(ctx,term,voted);
}
static int recv_test_begin(void *ctx)
{
    (void)ctx;recv_test.depth++;return EFS_OK;
}
static int recv_test_end(void *ctx)
{
    uint64_t term;
    CHECK(recv_test.depth > 0, "balanced receive batch");
    if (--recv_test.depth) return EFS_OK;
    recv_test.barriers++;
    if (recv_test.fail_end) return EFS_ERR_IO;
    return recv_test.original.last(ctx, &recv_test.durable, &term);
}
static int recv_test_append(void *ctx,uint64_t index,uint64_t term,
                            const uint8_t *cmd,uint32_t len)
{
    if(recv_test.fail_append)return EFS_ERR_IO;
    int rc=recv_test.original.append(ctx,index,term,cmd,len);
    if(rc==EFS_OK && !recv_test.depth){recv_test.durable=index;recv_test.barriers++;}
    return rc;
}
static int recv_test_send(void *ctx,const struct efs_raft_msg *m)
{
    (void)ctx;
    CHECK(!recv_test.depth,"no ACK before persistence");
    CHECK(!m->success || m->match_index<=recv_test.durable,"ACK covers durable bytes only");
    if (!recv_test.replies) recv_test.first_ack_applies=recv_test.applies;
    recv_test.replies++;
    recv_test.read_context=m->last_log_index;
    return EFS_OK;
}
static int recv_test_apply(void *ctx,uint64_t index,uint64_t term,
                           const uint8_t *cmd,uint32_t len)
{
    (void)ctx;(void)term;(void)cmd;(void)len;
    CHECK(!recv_test.depth && index<=recv_test.durable,"apply after durable boundary");
    if(recv_test.fail_apply) {
        int phase=recv_test.fail_apply--;
        return phase==2?EFS_ERR_IO:EFS_ERR_NOMEM;
    }
    recv_test.applies++;
    return EFS_OK;
}
static void test_follower_apply_retry(void)
{
    for(unsigned batch=0;batch<2;batch++) {
        memset(&recv_test,0,sizeof(recv_test));
        struct efs_raft_store *store=efs_raft_mem_create();struct efs_raft_store ops=*store;
        recv_test.original=ops;ops.append=recv_test_append;
        ops.batch_begin=recv_test_begin;ops.batch_end=recv_test_end;
        struct efs_raft_cfg cfg={0};cfg.id=1;cfg.n=3;cfg.election_ticks=10;cfg.heartbeat_ticks=1;
        cfg.store=&ops;cfg.store_ctx=store;cfg.send=recv_test_send;cfg.apply=recv_test_apply;
        struct efs_raft *r=efs_raft_new(&cfg);CHECK(r!=NULL,"apply retry follower");
        struct efs_raft_msg m={0};m.type=EFS_RAFT_MSG_AE_REQ;m.from=0;m.to=1;m.term=2;
        CHECK(efs_raft_recv(r,&m)==EFS_OK,"retry follower leader");
        m.nentries=1;m.entries[0].term=2;m.entries[0].cmd=(const uint8_t*)"c";m.entries[0].clen=1;m.leader_commit=1;
        recv_test.fail_apply=2;
        int rc=batch?efs_raft_recv_batch(r,&m,1):efs_raft_recv(r,&m);
        CHECK(rc==(batch?EFS_ERR_IO:EFS_OK) && !efs_raft_applied(r) && !recv_test.applies,"IO cannot advance apply or lose durable ACK");
        m.prev_index=1;m.prev_term=2;m.nentries=0;
        CHECK(efs_raft_recv(r,&m)==EFS_OK && !efs_raft_applied(r) && !recv_test.applies,"unchanged commit retries NOMEM without advancing");
        CHECK(efs_raft_recv(r,&m)==EFS_OK && efs_raft_applied(r)==1 && recv_test.applies==1,"unchanged heartbeat completes stalled durable entry");
        CHECK(efs_raft_recv(r,&m)==EFS_OK && recv_test.applies==1,"successful apply never repeated");
        efs_raft_free(r);efs_raft_mem_free(store);
    }
}
static void test_recv_batch(void)
{
    for(unsigned failure=0;failure<7;failure++) {
        memset(&recv_test,0,sizeof(recv_test));
        struct efs_raft_store *store=efs_raft_mem_create();
        struct efs_raft_store ops=*store;
        recv_test.original=ops;
        ops.append=recv_test_append;ops.batch_begin=recv_test_begin;ops.batch_end=recv_test_end;
        ops.save_hard=recv_test_hard;ops.truncate_from=recv_test_truncate;
        struct efs_raft_cfg cfg={0};
        cfg.id=1;cfg.n=3;cfg.election_ticks=10;cfg.heartbeat_ticks=1;
        cfg.store=&ops;cfg.store_ctx=store;cfg.send=recv_test_send;cfg.apply=recv_test_apply;
        struct efs_raft *r=efs_raft_new(&cfg);
        CHECK(r!=NULL,"receive fixture");
        struct efs_raft_msg msgs[3];memset(msgs,0,sizeof(msgs));
        msgs[0].type=EFS_RAFT_MSG_AE_REQ;msgs[0].from=0;msgs[0].to=1;msgs[0].term=2;
        CHECK(efs_raft_recv(r,&msgs[0])==EFS_OK,"establish stable leader");
        recv_test.replies=0;
        for(unsigned i=0;i<3;i++) {
            msgs[i]=msgs[0];msgs[i].prev_index=i;msgs[i].prev_term=i?2:0;
            msgs[i].nentries=1;msgs[i].entries[0].term=2;
            msgs[i].entries[0].cmd=(const uint8_t*)"a";msgs[i].entries[0].clen=1;
            msgs[i].leader_commit=i+1;msgs[i].last_log_index=10+i;
        }
        recv_test.fail_end=failure==1;recv_test.fail_append=failure==2;
        recv_test.fail_hard=failure==3 || failure==4;
        recv_test.fail_truncate=failure>=5;
        if(failure>=5){
            CHECK(recv_test.original.append(store,1,1,(const uint8_t*)"old",3)==EFS_OK,"conflicting tail seed");
            recv_test.durable=1;
        }
        if(failure==5){msgs[0].prev_index=1;msgs[0].prev_term=9;msgs[0].nentries=0;msgs[0].leader_commit=0;}
        if(failure==3)msgs[0].term=3; /* higher-term persistence fails */
        if(failure==4){msgs[0].type=EFS_RAFT_MSG_VOTE_REQ;msgs[0].from=2;}

        int rc=efs_raft_recv_batch(r,msgs,3);
        if(failure) {
            CHECK(rc==EFS_ERR_IO && !recv_test.replies && !recv_test.applies,
                  "failed batch neither ACKs nor applies");
            recv_test.fail_end=recv_test.fail_append=recv_test.fail_hard=recv_test.fail_truncate=0;
            CHECK(efs_raft_recv(r,&msgs[0])==EFS_ERR_IO && efs_raft_tick(r)==EFS_ERR_IO,
                  "failed persistence fences later heartbeats and elections");
        } else {
            CHECK(rc==EFS_OK && recv_test.barriers==1 && recv_test.replies==3 && recv_test.applies==3,
                  "three requests share one durable boundary");
            CHECK(recv_test.first_ack_applies==0,
                  "durable ACK is not held behind the cohort state-machine apply");
            CHECK(recv_test.read_context==12,"read context preserved");
            unsigned barriers=recv_test.barriers;
            /* Duplicate, then a new append, then a higher-term heartbeat:
             * neither duplicate rewrites nor a term update enters the batch. */
            msgs[0]=msgs[2];
            msgs[1]=msgs[2];msgs[1].prev_index=3;msgs[1].prev_term=2;msgs[1].leader_commit=4;
            msgs[2]=msgs[1];msgs[2].prev_index=4;msgs[2].nentries=0;msgs[2].term=3;
            CHECK(efs_raft_recv_batch(r,msgs,3)==EFS_OK,"flush before duplicate/term boundary");
            CHECK(recv_test.barriers==barriers+1 && recv_test.applies==4 && recv_test.replies==6,
                  "boundary ACKs/apply retain order");
            /* Two 600-KiB requests must not share a >1-MiB hold. */
            uint8_t *big=calloc(1,600*1024);
            CHECK(big!=NULL,"byte-bound fixture");
            if(big) {
                barriers=recv_test.barriers;
                for(unsigned j=0;j<2;j++) {
                    msgs[j]=msgs[2];msgs[j].nentries=1;msgs[j].prev_index=4+j;
                    msgs[j].prev_term=j?3:2;msgs[j].leader_commit=5+j;
                    msgs[j].entries[0].term=3;msgs[j].entries[0].cmd=big;
                    msgs[j].entries[0].clen=600*1024;
                }
                CHECK(efs_raft_recv_batch(r,msgs,2)==EFS_OK && recv_test.barriers==barriers+2,
                      "byte cap forces separate persistence boundaries");
                free(big);
            }
            CHECK(efs_raft_recv_batch(r,msgs,EFS_RAFT_RECV_MAX+1)==EFS_ERR_INVAL,
                  "request bound enforced");
        }
        CHECK(recv_test.depth==0,"all batch holds released");
        efs_raft_free(r);efs_raft_mem_free(store);
    }
}

/* Actual core: data/no-op/data share a barrier only when the middle request
 * proves no log change and carries no fresh read. Failure sends no ACK. */
static void test_recv_batch_noop(void)
{
    for (unsigned enabled=0; enabled<2; enabled++)
    for (unsigned kind=0; kind<4; kind++)
    for (unsigned fail=0; fail<2; fail++) {
        memset(&recv_test,0,sizeof(recv_test));
        struct efs_raft_store *store=efs_raft_mem_create();
        struct efs_raft_store ops=*store;recv_test.original=ops;
        ops.append=recv_test_append;ops.batch_begin=recv_test_begin;
        ops.batch_end=recv_test_end;
        struct efs_raft_cfg cfg={0};cfg.id=1;cfg.n=3;cfg.group=2;
        cfg.election_ticks=10;cfg.heartbeat_ticks=1;cfg.store=&ops;
        cfg.store_ctx=store;cfg.send=recv_test_send;cfg.apply=recv_test_apply;
        cfg.batch_noop=(int)enabled;
        struct efs_raft *r=efs_raft_new(&cfg);CHECK(r!=NULL,"noop fixture");
        struct efs_raft_msg m[3];memset(m,0,sizeof(m));
        m[0].type=EFS_RAFT_MSG_AE_REQ;m[0].from=0;m[0].to=1;
        m[0].group=2;m[0].term=2;
        CHECK(efs_raft_recv(r,&m[0])==EFS_OK,"noop establish leader");
        recv_test.replies=0;
        m[0].nentries=1;m[0].entries[0].term=2;
        m[0].entries[0].cmd=(const uint8_t*)"a";m[0].entries[0].clen=1;
        m[0].leader_commit=1;
        m[1]=m[0];m[1].nentries=0;m[1].vote_granted=1;
        if(kind==1)m[1]=m[0]; /* duplicate of pending data */
        if(kind==2)m[1].last_log_index=9; /* fresh read flushes */
        if(kind==3){m[1].prev_index=1;m[1].prev_term=99;} /* conflict */
        m[2]=m[0];m[2].prev_index=1;m[2].prev_term=2;m[2].leader_commit=2;
        recv_test.fail_end=(int)fail;
        int rc=efs_raft_recv_batch(r,m,3);
        if(fail) {
            CHECK(rc==EFS_ERR_IO && !recv_test.replies && !recv_test.applies,
                  "no-op cohort sync failure never ACKs or applies");
            recv_test.fail_end=0;
            CHECK(efs_raft_tick(r)==EFS_ERR_IO,"noop persistence failure fences election");
        } else if(kind==3) {
            CHECK(rc==EFS_ERR_PROTO,"conflict with committed prefix is not hidden in noop cohort");
        } else {
            CHECK(rc==EFS_OK && recv_test.replies==3 && recv_test.applies==2,
                  "noop replies and committed data preserved");
            CHECK(recv_test.barriers==(enabled && kind<2 ? 1u : 2u),
                  "only proven ordinary no-ops remove a durable boundary");
            CHECK(recv_test.first_ack_applies==0,"noop cohort ACK remains ahead of apply");
        }
        CHECK(!recv_test.depth,"noop hold released on every path");
        efs_raft_free(r);efs_raft_mem_free(store);
    }
}

static uint32_t overlap_admitted;
static uint32_t overlap_admit_max;
static int overlap_refuse;
static int overlap_admit(void *app,const struct efs_raft_entry *e,uint32_t n,int source)
{
    (void)app;(void)e;(void)source;overlap_admitted=n;
    if(n>overlap_admit_max)overlap_admit_max=n;
    return overlap_refuse?EFS_ERR_BUSY:EFS_OK;
}

/* Ordinary data + matching old prefix/new suffix must retain one barrier.
 * Exercise starting and extending holds, fresh reads, conflicts and EIO. */
static void test_recv_batch_overlap(void)
{
    for (unsigned enabled=0;enabled<2;enabled++)
    for (unsigned kind=0;kind<10;kind++) {
        memset(&recv_test,0,sizeof(recv_test));overlap_admitted=overlap_admit_max=0;overlap_refuse=kind==6;
        struct efs_raft_store *store=efs_raft_mem_create();
        struct efs_raft_store ops=*store;recv_test.original=ops;
        ops.append=recv_test_append;ops.batch_begin=recv_test_begin;ops.batch_end=recv_test_end;
        struct efs_raft_cfg cfg={0};cfg.id=1;cfg.n=3;cfg.group=2;cfg.election_ticks=10;cfg.heartbeat_ticks=1;
        cfg.store=&ops;cfg.store_ctx=store;cfg.send=recv_test_send;cfg.apply=recv_test_apply;
        cfg.batch_overlap=(int)enabled;cfg.admit=overlap_admit;
        struct efs_raft *r=efs_raft_new(&cfg);CHECK(r!=NULL,"overlap fixture");
        struct efs_raft_msg m[3];memset(m,0,sizeof(m));
        m[0].type=EFS_RAFT_MSG_AE_REQ;m[0].from=0;m[0].to=1;m[0].group=2;m[0].term=2;m[0].boot_id=2;
        CHECK(efs_raft_recv(r,m)==EFS_OK,"overlap leader established");recv_test.replies=0;
        m[0].nentries=1;m[0].entries[0]=(struct efs_raft_entry){2,1,(const uint8_t*)"a"};m[0].leader_commit=1;
        m[1]=m[0];m[1].nentries=2;m[1].entries[1]=(struct efs_raft_entry){2,1,(const uint8_t*)"b"};m[1].leader_commit=2;
        m[2]=m[0];m[2].prev_index=2;m[2].prev_term=2;m[2].leader_commit=3;
        if(kind==1)m[1].last_log_index=77;
        if(kind==2)m[1].entries[0].term=1; /* committed-prefix conflict */
        if(kind==3)recv_test.fail_end=1;
        if(kind==4){ /* overlapping request starts a hold from a durable seed */
            CHECK(recv_test.original.append(store,1,2,(const uint8_t*)"a",1)==EFS_OK,"overlap seed");
            recv_test.durable=1;
        }
        if(kind==5){m[1].term=3;m[1].entries[1].term=3;} /* changed term cannot join */
        if(kind==7)m[1].boot_id=1; /* delayed frame from a retired incarnation */
        if(kind==8){m[0].leader_commit=0;m[1].entries[0].term=1;} /* uncommitted conflict must flush/truncate */
        uint8_t *big=NULL;
        if(kind==9){
            big=calloc(1,600*1024);CHECK(big!=NULL,"overlap byte fixture");
            m[0].entries[0].cmd=big;m[0].entries[0].clen=600*1024;
            m[1].entries[0]=m[0].entries[0];m[1].entries[1].cmd=big;m[1].entries[1].clen=300*1024;
            m[2].entries[0].cmd=big;m[2].entries[0].clen=300*1024;
        }
        int rc=efs_raft_recv_batch(r,kind==4?m+1:m,kind==4?2:3);
        if(kind==2)CHECK(rc==EFS_ERR_PROTO,"overlap never hides committed conflict");
        else if(kind==3){CHECK(rc==EFS_ERR_IO && !recv_test.replies && !recv_test.applies,"overlap failed sync sends no ACK/apply");CHECK(efs_raft_tick(r)==EFS_ERR_IO,"overlap sync failure fenced");}
        else if(kind==5)CHECK(rc==EFS_OK,"changed term uses normal conflict path");
        else if(kind==6){uint64_t last,term;CHECK(rc==EFS_OK && ops.last(store,&last,&term)==EFS_OK && last==0,"refused suffix never persisted");}
        else if(kind==7){uint64_t last,term;CHECK(rc==EFS_OK && ops.last(store,&last,&term)==EFS_OK && last==1 && recv_test.applies==1,"retired boot frame cannot enter cohort or advance log");}
        else if(kind==8)CHECK(rc==EFS_OK && recv_test.barriers==3 && recv_test.applies==3,"uncommitted conflict keeps separate persistence/truncation boundaries");
        else {
            CHECK(rc==EFS_OK && recv_test.applies==3,"overlap applies exact committed suffix");
            CHECK(recv_test.barriers==(enabled?((kind==1||kind==9)?2u:1u):(kind==4?2u:3u)),"only verified non-read overlap joins durable boundaries");
            CHECK(overlap_admitted==1 && overlap_admit_max==1,"only novel suffix admitted");
        }
        CHECK(!recv_test.depth,"overlap releases every hold");
        efs_raft_free(r);efs_raft_mem_free(store);
        free(big);
    }
    overlap_refuse=0;
}

static int pressure_admit(void *app, const struct efs_raft_entry *e,
                           uint32_t n, int source)
{
    struct app *a=app;
    uint64_t count,bytes,logs;
    (void)e;
    if (a->pressure_mask & (1u<<source)) return EFS_ERR_BUSY;
    if (source==EFS_RAFT_ADMIT_LOCAL && a->pending_limit) {
        if (efs_raft_backlog(a->owner,&count,&bytes,&logs) != EFS_OK) return EFS_ERR_IO;
        if (count+n>a->pending_limit) return EFS_ERR_BUSY;
    }
    return EFS_OK;
}

static uint64_t pressure_command_work(const uint8_t *cmd, uint32_t n)
{
    return n && cmd[0]==42 ? 65536 : n;
}

static void test_pressure_quorum_and_owned_backlog(void)
{
    struct net net={.n=3};
    struct app app[3]={{0}};
    struct efs_raft_store *store[3];
    struct efs_raft_cfg cfg={0};
    int leader,follower;
    uint8_t cmd=42;
    uint64_t index=0,count=0,bytes=0,logs=0,last=0,term=0;
    cfg.n=3;cfg.send=send_now;cfg.net=&net;cfg.apply=apply_cmd;cfg.admit=pressure_admit;
    for (int i=0;i<3;i++) {
        store[i]=efs_raft_mem_create();cfg.id=i;cfg.store=store[i];cfg.store_ctx=store[i];
        cfg.app=&app[i];cfg.election_ticks=4+i*4;cfg.heartbeat_ticks=2;
        net.r[i]=efs_raft_new(&cfg);app[i].owner=net.r[i];
    }
    CHECK(elect(&net,40)==1,"pressure cluster elects");
    leader=leader_id(&net);follower=(leader+1)%3;
    CHECK(leader>=0,"pressure cluster leader");
    app[follower].pressure_mask=(1u<<EFS_RAFT_ADMIT_RECV)|(1u<<EFS_RAFT_ADMIT_LEAD);
    CHECK(store[follower]->last(store[follower],&last,&term)==EFS_OK,"paused follower tail");
    int batches=net.batches[follower];
    for (int i=0;i<100;i++) CHECK(efs_raft_propose(net.r[leader],&cmd,1,&index)==EFS_OK,
                                 "healthy quorum progresses with pressure follower");
    CHECK(efs_raft_applied(net.r[leader])==index,"healthy quorum applies all acknowledged work");
    CHECK(efs_raft_applied(net.r[follower])<=last,"pressure accepts no new suffix");
    CHECK(net.batches[follower]-batches<=2,"pressure backoff avoids per-proposal data storm");
    CHECK(elect(&net,80)==1 && leader_id(&net)==leader,"pressure follower does not disrupt healthy quorum");
    app[follower].pressure_mask=0;
    CHECK(elect(&net,40)==1,"released follower recovers");
    CHECK(efs_raft_applied(net.r[follower])==efs_raft_commit(net.r[leader]),"released follower converges");
    CHECK(app[follower].last==cmd,"recovery preserves command bytes");
    app[leader].pressure_mask=1u<<EFS_RAFT_ADMIT_LOCAL;
    CHECK(store[leader]->last(store[leader],&last,&term)==EFS_OK,"leader tail before refusal");
    CHECK(efs_raft_propose(net.r[leader],&cmd,1,&index)==EFS_ERR_BUSY,"pre-proposal refusal is explicit");
    uint64_t after;
    CHECK(store[leader]->last(store[leader],&after,&term)==EFS_OK && after==last,"refusal appended nothing");
    app[leader].pressure_mask=0;app[leader].pending_limit=3;
    for (int i=0;i<3;i++) if(i!=leader) net.drop[i]=1;
    for (int i=0;i<3;i++) CHECK(efs_raft_propose(net.r[leader],&cmd,1,&index)==EFS_OK,"accepted outstanding work");
    CHECK(efs_raft_backlog(net.r[leader],&count,&bytes,&logs)==EFS_OK && count==3 && bytes==3,
          "accepted log owns exact pending bytes independently of RPC lifetime");
    uint64_t work=0;
    CHECK(efs_raft_backlog_charge(net.r[leader],pressure_command_work,UINT64_MAX,&work)==EFS_OK && work==3*65536,
          "pending tiny commands retain expanded work independently of RPC lifetime");
    CHECK(efs_raft_propose(net.r[leader],&cmd,1,&index)==EFS_ERR_BUSY,"unapplied work consumes capacity");
    for (int i=0;i<3;i++) net.drop[i]=0;
    CHECK(elect(&net,40)==1,"quorum returns");
    CHECK(efs_raft_backlog(net.r[leader],&count,&bytes,&logs)==EFS_OK && !count && !bytes,
          "actual apply releases accepted capacity");
    CHECK(efs_raft_backlog_charge(net.r[leader],pressure_command_work,UINT64_MAX,&work)==EFS_OK && !work,
          "actual apply releases expanded pending work");
    app[leader].pressure_mask=1u<<EFS_RAFT_ADMIT_LEAD;
    CHECK(efs_raft_step_down(net.r[leader])==EFS_OK,"pressured leader relinquishes role");
    CHECK(elect(&net,80)==1 && leader_id(&net)!=leader,"healthy capacity wins leadership");
    for(int i=0;i<3;i++){efs_raft_free(net.r[i]);efs_raft_mem_free(store[i]);}
}

int main(void)
{
    test_read_quorum_before_apply();
    test_pressure_quorum_and_owned_backlog();
    test_follower_apply_retry();
    test_recv_batch();
    test_recv_batch_noop();
    test_recv_batch_overlap();
    test_election_i1();
    test_ae_full_window_not_resent_per_propose();
    test_step_down_no_campaign();
    test_replicate_and_readindex();
    test_readindex_outstanding_batch();
    test_transport_pressure_zero_probe();
    test_i3_i4_and_restart();
    test_snapshot();
    test_grow_3_to_5();
    test_four_voters();
    test_membership_persistence();
    test_grow_3_to_4();
    test_solo_durable_cap_then_grow();
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
