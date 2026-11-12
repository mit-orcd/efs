/* Task V4: the read-proof owner through its interface (src/server/read_proof.h).
 *
 * Links the real src/server/read_proof.c and src/server/group_view.c and
 * nothing else of the server. The only symbols it takes from elsewhere are the
 * Raft core getters (a fake core below, settable per case), the six host
 * functions of read_proof_host.h (fakes that count their calls and can run a
 * hook at the moment the owner calls them), and three library functions.
 *
 * One case per transition of the state table in read_proof.h and one case per
 * state for "the local replica stops leading". Cases run one request at a time;
 * a request that must sleep runs in a thread, and the main thread knows it is
 * inside its condition wait when it can take the wait lock (the V3 trick), then
 * publishes and wakes. No case sleeps. Exactly one case, test_deadline_
 * returns_busy, waits for the product's own bound (HOST_READ_TRIES x
 * HOST_TICK_US, 400 ms) once.
 *
 * Select cases by substring on the command line; --list prints the names.
 *
 * Not covered: the follower coalescer under concurrency (tests/test_readindex_
 * coalescer.py), the pair workers under saturation (tests/test_readindex_
 * remote_pair.py), the pump and the real Raft core (the fake core only
 * answers the getters), host stop (the owner has no stop cell: finding 1 of
 * docs/status/2026-11/read-proof-ownership-20261111.md), and the host's applied
 * wait, which is the host's. */
#include "read_proof.h"
#include "efs/wire.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define EFS_ERR_NOT_PRIMARY -15
#define GROUP 2 /* slot 1 */

/* ---- the fake Raft core ------------------------------------------------ */
struct efs_raft {
    uint64_t commit, applied, term, read_index, read_round, read_context;
    uint64_t proven_round, proven_index;
    int role, leader, read_pending, read_done, proven_done;
    uint32_t voters;
    int begin_fail, begins;
};

int efs_raft_role(const struct efs_raft *r) { return r->role; }
uint64_t efs_raft_term(const struct efs_raft *r) { return r->term; }
uint64_t efs_raft_commit(const struct efs_raft *r) { return r->commit; }
uint64_t efs_raft_applied(const struct efs_raft *r) { return r->applied; }
int efs_raft_leader(const struct efs_raft *r) { return r->leader; }
uint32_t efs_raft_voters(const struct efs_raft *r) { return r->voters; }
uint64_t efs_raft_read_round(const struct efs_raft *r) { return r->read_round; }
int efs_raft_read_pending(const struct efs_raft *r) { return r->read_pending; }
uint64_t efs_raft_read_index(const struct efs_raft *r) { return r->read_index; }
uint64_t efs_raft_read_context(const struct efs_raft *r) { return r->read_context; }
int efs_raft_read_quorum(const struct efs_raft *r) { return r->read_done; }
int efs_raft_read_proven(const struct efs_raft *r, uint64_t *round, uint64_t *index)
{
    *round = r->proven_round;
    *index = r->proven_index;
    return r->proven_done;
}
int efs_raft_read_begin(struct efs_raft *r)
{
    r->begins++;
    if (r->begin_fail)
        return r->begin_fail;
    r->read_round++;
    r->read_pending = 1;
    r->read_done = 0;
    return EFS_OK;
}

/* ---- library stubs ----------------------------------------------------- */
uint64_t efs_net_deadline_ms(void) { return 0; }
void efs_net_set_deadline_ms(uint64_t v) { (void)v; }
uint64_t efs_batch_obs_now_us(void) { return 0; }
void efs_telemetry_log(int critical, const char *f, ...) { (void)critical; (void)f; }
void efs_telemetry_printf(struct efs_telemetry *t, const char *f, ...) { (void)t; (void)f; }

/* ---- the fake host ----------------------------------------------------- */
struct efs_raft_host {
    struct read_proof rp;
    struct group_views *gv;
    struct efs_raft core;
    int raft_id, members;
    /* gate */
    int gate_block, gate_hint, gate_calls;
    /* remote read */
    int remote_rc, remote_hint, remote_calls, remote_leader, kicks_at_remote;
    struct efs_raft core0; /* group 0, slot 0 */
    uint64_t remote_index;
    /* kick */
    int kicks;
    /* the host's applied wait */
    int applied_rc, applied_calls;
    uint64_t applied_idx;
    /* a hook run when the owner calls the host (at the call number) */
    void (*on_kick)(struct efs_raft_host *);
    void (*on_remote)(struct efs_raft_host *);
    void (*on_gate)(struct efs_raft_host *);
    int on_gate_call;
};

int read_proof_host_gate(struct efs_raft_host *h, uint8_t group, int *hint)
{
    (void)group;
    int n = __atomic_add_fetch(&h->gate_calls, 1, __ATOMIC_SEQ_CST);
    if (h->on_gate && n == h->on_gate_call)
        h->on_gate(h);
    if (!h->gate_block)
        return 0;
    *hint = h->gate_hint;
    return 1;
}

int read_proof_host_remote(struct efs_raft_host *h, uint8_t group, int leader,
                           struct efs_msg_raft_mkfs_reply *rep)
{
    (void)group;
    __atomic_add_fetch(&h->remote_calls, 1, __ATOMIC_SEQ_CST);
    h->remote_leader = leader;
    h->kicks_at_remote = h->kicks;
    if (h->on_remote)
        h->on_remote(h);
    if (h->remote_rc == EFS_OK) {
        rep->index = h->remote_index;
        rep->leader_hint = h->remote_hint;
    }
    return h->remote_rc;
}

void read_proof_host_kick(struct efs_raft_host *h)
{
    h->kicks++;
    if (h->on_kick)
        h->on_kick(h);
}

int read_proof_host_wait_applied(struct efs_raft_host *h, uint8_t group,
                                 uint64_t idx, int *hint)
{
    (void)group; (void)hint;
    __atomic_add_fetch(&h->applied_calls, 1, __ATOMIC_SEQ_CST);
    h->applied_idx = idx;
    return h->applied_rc;
}

int read_prof_on(void) { return 0; }
void read_prof_add(struct read_prof_acc *a, uint64_t us) { (void)a; (void)us; }

/* ---- fixtures ---------------------------------------------------------- */
static struct efs_raft_host *new_host(void)
{
    struct efs_raft_host *h = calloc(1, sizeof(*h));

    assert(h);
    h->gv = calloc(1, sizeof(*h->gv));
    assert(h->gv);
    group_view_init(h->gv);
    group_view_attach(h->gv, 0, 0);
    group_view_attach(h->gv, 1, GROUP);
    h->raft_id = 0;
    h->members = 4;
    read_proof_init(&h->rp, h, h->gv, &h->raft_id, &h->members);
    read_proof_attach(&h->rp, 0, 0);
    read_proof_attach(&h->rp, 1, GROUP);
    h->remote_rc = EFS_OK;
    h->remote_index = 7;
    h->remote_hint = 1;
    /* a leader at term 3, commit 10, last finished round 4 with nothing owed */
    h->core = (struct efs_raft){.commit = 10, .applied = 10, .term = 3,
        .read_index = 10, .read_round = 4, .read_context = 7, .proven_round = 4,
        .proven_index = 10, .role = EFS_RAFT_LEADER, .leader = 0, .read_pending = 0,
        .read_done = 1, .proven_done = 1, .voters = 15};
    return h;
}

static void free_host(struct efs_raft_host *h)
{
    group_view_destroy(h->gv);
    free(h->gv);
    free(h);
}

static void publish(struct efs_raft_host *h)
{
    host_publish_view(h->gv, 1, &h->core);
}

static void publish_and_wake(struct efs_raft_host *h)
{
    publish(h);
    host_waiters_wake(h->gv);
}

static struct efs_raft *cores_of(struct efs_raft_host *h, struct efs_raft *out[2], int hosted)
{
    out[0] = NULL;
    out[1] = hosted ? &h->core : NULL;
    return out[1];
}

/* A view in which round `round` finished with `index` and the reader's own
 * apply stands at `applied`. */
static void finish_round(struct efs_raft_host *h, uint64_t round, uint64_t index,
                         uint64_t applied)
{
    h->core.read_round = round;
    h->core.read_index = index;
    h->core.read_done = 1;
    h->core.read_pending = 0;
    h->core.proven_round = round;
    h->core.proven_index = index;
    h->core.proven_done = 1;
    h->core.applied = applied;
    h->core.commit = index > h->core.commit ? index : h->core.commit;
}

/* A request in its own thread. */
struct req {
    struct efs_raft_host *h;
    int proof_only, rc, hint;
    uint64_t index;
    pthread_t t;
};

static void *req_main(void *p)
{
    struct req *q = p;

    q->hint = -2;
    q->rc = q->proof_only
        ? read_proof_prove(&q->h->rp, GROUP, &q->hint, &q->index)
        : read_proof_read(&q->h->rp, GROUP, &q->hint);
    return NULL;
}

static void req_start(struct req *q, struct efs_raft_host *h, int proof_only)
{
    memset(q, 0, sizeof(*q));
    q->h = h;
    q->proof_only = proof_only;
    assert(!pthread_create(&q->t, NULL, req_main, q));
}

static void req_join(struct req *q)
{
    assert(!pthread_join(q->t, NULL));
}

/* Return once the request is inside its condition wait: it holds the wait
 * lock until the wait releases it, so the lock is granted only then. */
static void wait_asleep(struct efs_raft_host *h)
{
    /* bounded: a reader that returned early would otherwise spin us forever */
    for (long i = 0; !group_view_waiting(h->gv); i++) {
        assert(i < 2000000L && "the reader is not asleep (it returned early)");
        sched_yield();
    }
    group_view_wait_lock(h->gv);
    group_view_wait_unlock(h->gv);
}

/* ---- S0: gate and replica ---------------------------------------------- */
static void test_gate_refuses_incompatible_group(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    publish(h);
    h->gate_block = 1;
    h->gate_hint = 5;
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_NOT_PRIMARY && hint == 5);
    assert(h->gate_calls == 1 && !h->kicks && !h->remote_calls);
    free_host(h);
}

static void test_no_replica_refuses(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    /* nothing published: the slot has no replica */
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_NOT_PRIMARY && hint == -1);
    assert(!h->kicks && !h->remote_calls);
    free_host(h);
}

/* ---- S1: follower ------------------------------------------------------ */
static void test_follower_asks_leader_and_applies(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    h->core.role = EFS_RAFT_FOLLOWER;
    h->core.leader = 2;
    publish(h);
    h->remote_index = 7;
    h->remote_hint = 2;
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_OK);
    assert(h->remote_calls == 1 && h->applied_calls == 1 && h->applied_idx == 7);
    assert(hint == 2 && !h->kicks && h->remote_leader == 2);
    /* leader unknown: the call is still made, with -1 */
    h->core.leader = -1;
    publish(h);
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_OK && h->remote_leader == -1);
    free_host(h);
}

static void test_follower_proof_endpoint_never_forwards(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;
    uint64_t idx = 99;

    h->core.role = EFS_RAFT_FOLLOWER;
    h->core.leader = 2;
    publish(h);
    assert(read_proof_prove(&h->rp, GROUP, &hint, &idx) == EFS_ERR_NOT_PRIMARY);
    assert(hint == 2 && idx == 0 && !h->remote_calls);
    free_host(h);
}

static void test_follower_leader_is_self_refuses(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    h->core.role = EFS_RAFT_FOLLOWER;
    h->core.leader = 0; /* this member */
    publish(h);
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_NOT_PRIMARY && hint == 0);
    assert(!h->remote_calls);
    free_host(h);
}

/* ---- S9/S11: asking the leader ----------------------------------------- */
static void test_follower_busy_apply_names_another_leader(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    h->core.role = EFS_RAFT_FOLLOWER;
    h->core.leader = 2;
    publish(h);
    h->applied_rc = EFS_ERR_BUSY; /* the host's applied wait ran out */
    h->remote_hint = 3;           /* the leader named another member */
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_NOT_PRIMARY && hint == 3);
    h->remote_hint = 0;           /* itself: no redirect, the BUSY stands */
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_BUSY);
    h->remote_hint = 9;           /* not a member */
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_BUSY);
    h->remote_rc = EFS_ERR_BUSY;  /* the RPC failed: no applied wait at all */
    h->applied_calls = 0;
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_BUSY && !h->applied_calls);
    free_host(h);
}

/* ---- S2: admission ----------------------------------------------------- */
static void test_admission_takes_view_commit_and_next_round(void)
{
    /* The view shows round 4 finished at commit 10. A reader arriving now is
     * admitted with want = 10 and round = 5: round 4's probes may precede it,
     * so it must not return before round 5 has a quorum. */
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct req q;

    publish(h);
    req_start(&q, h, 1);
    wait_asleep(h);
    assert(h->kicks == 1 && h->core.read_round == 4); /* the reader begins nothing itself */
    cores_of(h, cores, 1);
    assert(read_proof_start_requested(&h->rp, cores) == 2u && h->core.read_round == 5);
    /* round 5 finishes at index 12 >= want: the reader is answered by round 5 */
    finish_round(h, 5, 12, 10);
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_OK && q.index == 12 && q.hint == 0);
    free_host(h);
}

static void test_admission_overflow_is_busy(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    h->core.read_round = UINT64_MAX - 1;
    publish(h);
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_BUSY);
    assert(!h->kicks);
    free_host(h);
}

/* ---- S3: request ------------------------------------------------------- */
static void test_request_is_raised_once_and_kicks(void)
{
    /* The view says round 4 began (the admission reads it) and shows round 5
     * already proven, so each request returns at its first check. Readers of
     * one round share the raise and the kick; the next round needs a new one. */
    struct efs_raft_host *h = new_host();
    int hint = -9;
    uint64_t idx = 0;

    h->core.read_round = 4;
    h->core.proven_round = 5;
    h->core.proven_index = 12;
    publish(h);
    assert(read_proof_prove(&h->rp, GROUP, &hint, &idx) == EFS_OK && idx == 12);
    assert(read_proof_prove(&h->rp, GROUP, &hint, &idx) == EFS_OK && idx == 12);
    assert(h->kicks == 1);
    h->core.read_round = 5;
    h->core.proven_round = 6;
    publish(h);
    assert(read_proof_prove(&h->rp, GROUP, &hint, &idx) == EFS_OK);
    assert(h->kicks == 2);
    free_host(h);
}

/* ---- S4: the pump starts the round -------------------------------------- */
static void raise_request(struct efs_raft_host *h)
{
    /* a reader that asked for round 5 and went to sleep */
    struct req q;
    publish(h);
    req_start(&q, h, 1);
    wait_asleep(h);
    h->core.role = EFS_RAFT_FOLLOWER; /* ends the reader: published, woken */
    publish_and_wake(h);
    req_join(&q);
    h->core.role = EFS_RAFT_LEADER;
    publish(h);
}

static void test_start_requested_begins_when_previous_quorum_done(void)
{
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    uint64_t stamp;
    struct host_view v;

    raise_request(h);
    cores_of(h, cores, 1);
    host_view_get(h->gv, GROUP, &v);
    stamp = v.stamp;
    assert(read_proof_start_requested(&h->rp, cores) == 2u);
    assert(h->core.read_round == 5 && h->core.begins == 1);
    host_view_get(h->gv, GROUP, &v);
    assert(v.stamp != stamp && v.read_round == 5 && v.read_pending); /* published */
    /* the request is satisfied: nothing more to start */
    assert(read_proof_start_requested(&h->rp, cores) == 0u && h->core.begins == 1);
    free_host(h);
}

static void test_start_requested_skips_unled_and_unhosted(void)
{
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];

    raise_request(h);
    /* not hosted: the host hands NULL */
    cores_of(h, cores, 0);
    assert(read_proof_start_requested(&h->rp, cores) == 0u && !h->core.begins);
    /* the previous round has no quorum yet: one round in flight */
    cores_of(h, cores, 1);
    h->core.read_pending = 1;
    h->core.read_done = 0;
    assert(read_proof_start_requested(&h->rp, cores) == 0u && !h->core.begins);
    h->core.read_pending = 0;
    h->core.read_done = 1;
    /* a follower is skipped and the request stays raised */
    h->core.role = EFS_RAFT_FOLLOWER;
    assert(read_proof_start_requested(&h->rp, cores) == 0u && !h->core.begins);
    h->core.role = EFS_RAFT_LEADER;
    assert(read_proof_start_requested(&h->rp, cores) == 2u && h->core.begins == 1);
    free_host(h);
}

static void test_failed_begin_publishes_nothing_and_is_retried(void)
{
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct host_view v;
    uint64_t stamp;

    raise_request(h);
    cores_of(h, cores, 1);
    host_view_get(h->gv, GROUP, &v);
    stamp = v.stamp;
    h->core.begin_fail = EFS_ERR_BUSY;
    assert(read_proof_start_requested(&h->rp, cores) == 0u && h->core.begins == 1);
    host_view_get(h->gv, GROUP, &v);
    assert(v.stamp == stamp && h->core.read_round == 4);
    h->core.begin_fail = 0;
    assert(read_proof_start_requested(&h->rp, cores) == 2u && h->core.read_round == 5);
    free_host(h);
}

/* ---- S5: check ---------------------------------------------------------- */
static void test_read_returns_ok_once_applied_reaches_the_floor(void)
{
    struct efs_raft_host *h = new_host();
    struct req q;
    struct efs_raft *cores[2];

    publish(h);
    req_start(&q, h, 0);
    wait_asleep(h);
    cores_of(h, cores, 1);
    assert(read_proof_start_requested(&h->rp, cores) == 2u);
    finish_round(h, 5, 12, 12);
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_OK && q.hint == 0);
    free_host(h);
}

static void test_frozen_floor_survives_a_later_round(void)
{
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct req q;

    publish(h);
    req_start(&q, h, 0);
    wait_asleep(h);
    cores_of(h, cores, 1);
    assert(read_proof_start_requested(&h->rp, cores) == 2u);
    /* round 5 has its quorum at 12 but this member applied only 8: the floor
     * freezes at 12 and the reader keeps waiting */
    finish_round(h, 5, 12, 8);
    publish_and_wake(h);
    /* a later round with a far higher index must not move the target */
    finish_round(h, 6, 40, 12);
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_OK);
    free_host(h);
}

static void test_proof_only_returns_quorum_index_before_apply(void)
{
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct req q;

    publish(h);
    req_start(&q, h, 1);
    wait_asleep(h);
    cores_of(h, cores, 1);
    assert(read_proof_start_requested(&h->rp, cores) == 2u);
    finish_round(h, 5, 12, 3); /* applied far behind */
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_OK && q.index == 12);
    free_host(h);
}

static void test_context_change_refuses(void)
{
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct req q;

    publish(h);
    req_start(&q, h, 0);
    wait_asleep(h);
    cores_of(h, cores, 1);
    assert(read_proof_start_requested(&h->rp, cores) == 2u);
    finish_round(h, 5, 12, 8);   /* floor freezes with context 7 */
    publish_and_wake(h);
    h->core.read_context = 8;    /* configuration change */
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_ERR_NOT_PRIMARY);
    free_host(h);
}

static void test_term_change_refuses(void)
{
    struct efs_raft_host *h = new_host();
    struct req q;

    publish(h);
    req_start(&q, h, 0);
    wait_asleep(h);
    h->core.term = 4; /* re-elected: same role, new term */
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_ERR_NOT_PRIMARY && h->remote_calls == 0);
    free_host(h);
}

/* ---- S6: asleep --------------------------------------------------------- */
static void test_waits_in_slot_and_wakes_on_publish(void)
{
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct req q;

    publish(h);
    req_start(&q, h, 1);
    wait_asleep(h);
    assert(group_view_waiting(h->gv) == 1);
    /* a publish that changes nothing the reader waits for leaves it asleep */
    h->core.commit++;
    publish_and_wake(h);
    assert(group_view_waiting(h->gv) == 1);
    cores_of(h, cores, 1);
    assert(read_proof_start_requested(&h->rp, cores) == 2u);
    finish_round(h, 5, 12, 12);
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_OK && !group_view_waiting(h->gv));
    free_host(h);
}

static void test_replica_gone_while_asleep(void)
{
    struct efs_raft_host *h = new_host();
    struct req q;

    publish(h);
    req_start(&q, h, 0);
    wait_asleep(h);
    host_publish_view(h->gv, 1, NULL); /* the core is gone */
    host_waiters_wake(h->gv);
    req_join(&q);
    assert(q.rc == EFS_ERR_NOT_PRIMARY && !h->remote_calls);
    free_host(h);
}

/* ---- deadline ------------------------------------------------------------ */
static void test_deadline_returns_busy(void)
{
    struct efs_raft_host *h = new_host();
    struct timespec a, b;
    int hint = -9;
    long ms;

    publish(h);
    clock_gettime(CLOCK_MONOTONIC, &a);
    /* the product's own bound, once: nothing publishes, nothing wakes */
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_BUSY);
    clock_gettime(CLOCK_MONOTONIC, &b);
    ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    assert(ms >= (long)HOST_READ_TRIES * HOST_TICK_US / 1000 - 5);
    assert(ms < 5000);
    free_host(h);
}

/* ---- the local replica stops leading, one case per state ----------------- */
static void follower_view(struct efs_raft_host *h)
{
    h->core.role = EFS_RAFT_FOLLOWER;
    h->core.leader = 2;
    publish(h);
}

static void test_loses_leadership_after_admission(void)
{
    /* S3: leadership goes while the reader raises its request (the kick): the
     * next pass sees a follower and takes S1 */
    struct efs_raft_host *h = new_host();
    int hint = -9;
    uint64_t idx = 9;

    publish(h);
    h->on_kick = follower_view;
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_OK);
    assert(h->kicks == 1 && h->remote_calls == 1);
    free_host(h);
    /* the proof endpoint never forwards */
    h = new_host();
    publish(h);
    h->on_kick = follower_view;
    assert(read_proof_prove(&h->rp, GROUP, &hint, &idx) == EFS_ERR_NOT_PRIMARY);
    assert(idx == 0 && !h->remote_calls);
    free_host(h);
}

static void test_loses_leadership_after_request(void)
{
    /* S4: the round was requested and the pump finds a core that no longer
     * leads: it skips it, the request stays raised, the reader is woken */
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct req q;

    publish(h);
    req_start(&q, h, 1);
    wait_asleep(h);
    h->core.role = EFS_RAFT_FOLLOWER;
    cores_of(h, cores, 1);
    assert(read_proof_start_requested(&h->rp, cores) == 0u && !h->core.begins);
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_ERR_NOT_PRIMARY);
    free_host(h);
}

static void test_loses_leadership_while_checking_with_floor(void)
{
    /* S5: the proof was seen (floor frozen, apply owed) and then the replica
     * stops leading: the proof is not returned; a read asks the new leader,
     * the proof endpoint is refused */
    for (int proof_only = 0; proof_only <= 1; proof_only++) {
        struct efs_raft_host *h = new_host();
        struct efs_raft *cores[2];
        struct req q;

        publish(h);
        req_start(&q, h, proof_only);
        wait_asleep(h);
        cores_of(h, cores, 1);
        assert(read_proof_start_requested(&h->rp, cores) == 2u);
        finish_round(h, 5, 12, 8);
        if (proof_only) {
            /* proof_only returns at the quorum: the floor never waits */
            finish_round(h, 5, 12, 3);
        } else {
            /* the finished round is published while apply is still owed (8 < 12):
             * the pump's scan freezes the floor in the slot and does not wake
             * the reader, which stays asleep (it would have returned if the
             * proof counted as complete); only then does the role flip. The
             * frozen floor itself is private: test_frozen_floor_survives_a_
             * later_round pins it. */
            publish_and_wake(h);
            wait_asleep(h); /* published, not enough: still asleep */
            assert(!h->remote_calls && !h->applied_calls);
        }
        h->core.role = EFS_RAFT_FOLLOWER;
        h->core.leader = 2;
        publish_and_wake(h);
        req_join(&q);
        if (proof_only)
            assert(q.rc == EFS_ERR_NOT_PRIMARY && !h->remote_calls);
        else
            assert(q.rc == EFS_OK && h->remote_calls == 1 && h->applied_idx == 7);
        free_host(h);
    }
}

static void test_loses_leadership_while_asleep(void)
{
    /* S6: asleep with nothing owed; the role changes */
    struct efs_raft_host *h = new_host();
    struct req q;

    publish(h);
    req_start(&q, h, 0);
    wait_asleep(h);
    follower_view(h);
    host_waiters_wake(h->gv);
    req_join(&q);
    assert(q.rc == EFS_OK && h->remote_calls == 1 && h->applied_calls == 1);
    free_host(h);
}

static void gain_leadership(struct efs_raft_host *h)
{
    h->core.role = EFS_RAFT_LEADER;
    h->core.leader = 0;
    publish(h);
}

static void test_leadership_changes_while_asking_leader(void)
{
    /* S9: asked while following; the replica becomes the leader during the
     * RPC. The owner does not look again: the answer is the RPC's, followed by
     * the host's applied wait. */
    struct efs_raft_host *h = new_host();
    int hint = -9;

    follower_view(h);
    h->on_remote = gain_leadership;
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_OK);
    assert(h->remote_calls == 1 && h->applied_calls == 1 && h->applied_idx == 7 && !h->kicks);
    free_host(h);
}


static void block_gate(struct efs_raft_host *h)
{
    h->gate_block = 1;
    h->gate_hint = 5;
}

static void new_term_follower(struct efs_raft_host *h)
{
    h->core.term = 4;
    h->core.role = EFS_RAFT_FOLLOWER;
    h->core.leader = 2;
    publish(h);
}

struct pairreq {
    struct efs_raft_host *h;
    int rc, hint;
    pthread_t t;
};

static void *pair_main(void *p)
{
    struct pairreq *q = p;

    q->rc = read_proof_read_pair(&q->h->rp, 0, GROUP, &q->hint);
    return NULL;
}

static void pair_start(struct pairreq *q, struct efs_raft_host *h)
{
    memset(q, 0, sizeof(*q));
    q->h = h;
    assert(!pthread_create(&q->t, NULL, pair_main, q));
}

/* ---- what the admission and the check compare against ------------------- */
static void test_proof_must_cover_the_arrival_commit(void)
{
    /* want = the commit seen on arrival (10). A finished round 5 whose index is
     * 9 does not cover it; round 6 at 11 does. */
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct req q;

    publish(h);
    req_start(&q, h, 1);
    wait_asleep(h);
    cores_of(h, cores, 1);
    assert(read_proof_start_requested(&h->rp, cores) == 2u);
    finish_round(h, 5, 9, 9);
    h->core.commit = 10;
    publish_and_wake(h);
    wait_asleep(h); /* published, not enough: still asleep */
    finish_round(h, 6, 11, 11);
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_OK && q.index == 11);
    free_host(h);
}

static void test_begun_round_ahead_of_the_view_is_waited_for(void)
{
    /* A round was begun (the owner's begun counter = 6) that the published view does not show
     * yet (read_round 4). The reader needs round 7: a proof of round 6 is
     * not enough. */
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct req q;

    publish(h);
    h->core.read_round = 5;
    assert(read_proof_begin(&h->rp, GROUP, &h->core) == EFS_OK); /* counter = 6 */
    h->core.read_round = 4;
    h->core.read_pending = 0;
    req_start(&q, h, 1);
    wait_asleep(h);
    assert(h->kicks == 1);
    finish_round(h, 6, 12, 12);
    publish_and_wake(h);
    wait_asleep(h); /* published, not enough: still asleep */
    cores_of(h, cores, 1);
    finish_round(h, 7, 13, 13);
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_OK && q.index == 13);
    free_host(h);
}

static void test_read_covered_by_round_waits_for_apply(void)
{
    /* read_epoch 0 (no context): no floor is frozen, and the covering-round
     * test applies: the proof is used only once applied reaches it. */
    struct efs_raft_host *h = new_host();
    struct req q;

    h->core.read_context = 0;
    publish(h);
    req_start(&q, h, 0);
    wait_asleep(h);
    finish_round(h, 5, 12, 8);
    h->core.read_context = 0;
    publish_and_wake(h);
    wait_asleep(h); /* published, not enough: still asleep */
    assert(!h->remote_calls);
    h->core.applied = 12;
    publish_and_wake(h);
    req_join(&q);
    assert(q.rc == EFS_OK && !h->remote_calls);
    free_host(h);
}

static void test_gate_refuses_on_a_later_pass(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    publish(h);
    h->on_gate = block_gate;
    h->on_gate_call = 2; /* the first call is before the loop */
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_NOT_PRIMARY && hint == 5);
    assert(h->gate_calls == 2 && !h->kicks && !h->remote_calls);
    free_host(h);
}

static void test_new_term_on_the_admission_pass_refuses_with_the_old_hint(void)
{
    /* The view copy of the pass says leader, term 3, leader 0 (this member).
     * The gate hook publishes term 4, a follower, leader 2 before the
     * admission. The re-read after admission shows term 4: only the term is
     * tested there; the hint is the older copy's. */
    struct efs_raft_host *h = new_host();
    int hint = -9;

    publish(h);
    h->on_gate = new_term_follower;
    h->on_gate_call = 2;
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_ERR_NOT_PRIMARY);
    assert(hint == 0 && h->kicks == 1 && !h->remote_calls);
    free_host(h);
}

static void test_role_change_in_the_same_term_is_acted_on_at_the_next_pass(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    publish(h);
    h->on_gate = follower_view; /* same term 3, now a follower of member 2 */
    h->on_gate_call = 2;
    assert(read_proof_read(&h->rp, GROUP, &hint) == EFS_OK);
    assert(h->kicks == 1 && h->remote_calls == 1 && h->remote_leader == 2);
    free_host(h);
}

/* ---- read_proof_read_pair ------------------------------------------------ */
static void publish0(struct efs_raft_host *h)
{
    host_publish_view(h->gv, 0, &h->core0);
}

static void test_pair_without_a_replica_uses_the_scalar_path(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    publish(h); /* group 2 hosted, group 0 not */
    assert(read_proof_read_pair(&h->rp, 0, GROUP, &hint) == EFS_ERR_NOT_PRIMARY);
    assert(hint == -1 && !h->remote_calls && !h->kicks);
    free_host(h);
}

static void test_pair_of_followers_asks_both_leaders(void)
{
    struct efs_raft_host *h = new_host();
    int hint = -9;

    h->core0 = h->core;
    h->core0.role = EFS_RAFT_FOLLOWER;
    h->core0.leader = 3;
    h->core.role = EFS_RAFT_FOLLOWER;
    h->core.leader = 2;
    publish0(h);
    publish(h);
    assert(read_proof_read_pair(&h->rp, 0, GROUP, &hint) == EFS_OK);
    assert(h->remote_calls == 2 && h->applied_calls == 2 && !h->kicks);
    free_host(h);
}

static void note_kicks(struct efs_raft_host *h) { (void)h; }

static void test_pair_requests_the_led_group_before_asking_the_other(void)
{
    /* group 0 follows (asks its leader), group 2 leads: both are admitted
     * and the round of group 2 is requested before the remote call. */
    struct efs_raft_host *h = new_host();
    struct efs_raft *cores[2];
    struct pairreq q;

    h->core0 = h->core;
    h->core0.role = EFS_RAFT_FOLLOWER;
    h->core0.leader = 3;
    publish0(h);
    publish(h);
    h->on_kick = note_kicks;
    pair_start(&q, h);
    wait_asleep(h);
    assert(h->kicks == 1 && h->remote_calls == 1 && h->kicks_at_remote == 1);
    cores[0] = NULL;
    cores[1] = &h->core;
    assert(read_proof_start_requested(&h->rp, cores) == 2u);
    finish_round(h, 5, 12, 12);
    publish_and_wake(h);
    assert(!pthread_join(q.t, NULL));
    assert(q.rc == EFS_OK);
    free_host(h);
}

/* ---- driver --------------------------------------------------------------- */
static int g_argc, g_list;
static char **g_argv;
static char *g_used;

static int run_case(const char *name)
{
    int hit = g_argc == 1;

    if (g_list) {
        puts(name);
        return 0;
    }
    for (int i = 1; i < g_argc; i++)
        if (strstr(name, g_argv[i]))
            hit = g_used[i] = 1;
    return hit;
}
#define RUN(fn) do { if (run_case(#fn)) fn(); } while (0)

int main(int argc, char **argv)
{
    int bad = 0;

    g_argc = argc;
    g_argv = argv;
    g_list = argc == 2 && !strcmp(argv[1], "--list");
    g_used = calloc((size_t)argc, 1);
    if (!g_used)
        abort();
    RUN(test_gate_refuses_incompatible_group);
    RUN(test_no_replica_refuses);
    RUN(test_follower_asks_leader_and_applies);
    RUN(test_follower_proof_endpoint_never_forwards);
    RUN(test_follower_leader_is_self_refuses);
    RUN(test_follower_busy_apply_names_another_leader);
    RUN(test_admission_takes_view_commit_and_next_round);
    RUN(test_admission_overflow_is_busy);
    RUN(test_request_is_raised_once_and_kicks);
    RUN(test_start_requested_begins_when_previous_quorum_done);
    RUN(test_start_requested_skips_unled_and_unhosted);
    RUN(test_failed_begin_publishes_nothing_and_is_retried);
    RUN(test_read_returns_ok_once_applied_reaches_the_floor);
    RUN(test_frozen_floor_survives_a_later_round);
    RUN(test_proof_only_returns_quorum_index_before_apply);
    RUN(test_context_change_refuses);
    RUN(test_term_change_refuses);
    RUN(test_waits_in_slot_and_wakes_on_publish);
    RUN(test_replica_gone_while_asleep);
    RUN(test_loses_leadership_after_admission);
    RUN(test_loses_leadership_after_request);
    RUN(test_loses_leadership_while_checking_with_floor);
    RUN(test_loses_leadership_while_asleep);
    RUN(test_leadership_changes_while_asking_leader);
    RUN(test_proof_must_cover_the_arrival_commit);
    RUN(test_begun_round_ahead_of_the_view_is_waited_for);
    RUN(test_read_covered_by_round_waits_for_apply);
    RUN(test_gate_refuses_on_a_later_pass);
    RUN(test_new_term_on_the_admission_pass_refuses_with_the_old_hint);
    RUN(test_role_change_in_the_same_term_is_acted_on_at_the_next_pass);
    RUN(test_pair_without_a_replica_uses_the_scalar_path);
    RUN(test_pair_of_followers_asks_both_leaders);
    RUN(test_pair_requests_the_led_group_before_asking_the_other);
    RUN(test_deadline_returns_busy);
    if (g_list)
        return 0;
    for (int i = 1; i < argc; i++)
        if (!g_used[i]) {
            fprintf(stderr, "%s: no case matches '%s' (see --list)\n", argv[0], argv[i]);
            bad = 1;
        }
    if (bad)
        return 2;
    puts("read_proof interface: gate and replica, follower and leader RPC, admission, request, "
         "pump start, floor, context and term, sleeping and waking, deadline, leadership loss "
         "in each state PASS");
    return 0;
}
