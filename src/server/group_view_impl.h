#ifndef EFS_SERVER_GROUP_VIEW_IMPL_H
#define EFS_SERVER_GROUP_VIEW_IMPL_H

#ifndef EFS_SERVER_GROUP_VIEW_H
#error "group_view_impl.h is private: include group_view.h"
#endif

/* PRIVATE. This file is the owner's representation and the bodies of its
 * inline operations. A caller never needs to read it and must not include it
 * directly; group_view.h includes it as its last line. Nothing in it is a
 * promise: the contract is group_view.h. The bodies are inline because they
 * run on every metadata request. */

#define V_LOAD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define V_STORE(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* One sleeping handler. All fields are read and written under cv_mu. */
struct host_waiter {
    pthread_cond_t cv;
    uint64_t idx;   /* wake once the group's applied >= idx */
    uint64_t seen;  /* any=1: wake once the group's view stamp != seen */
    uint64_t round, term; /* any=3: request-scoped fresh read */
    struct host_read_floor floor; /* first sufficient proof for this sleeper */
    uint8_t notified, proof_only;
    uint8_t group;
    uint8_t active;
    uint8_t any;
};

/* private: one group's published view. Written by host_publish_view under
 * view_mu, read with atomic loads. */
struct group_view_slot {
    uint8_t group;      /* the group id of this slot, set by group_view_attach */
    uint64_t v_seq; /* odd while publishing; protects the entire read predicate */
    uint64_t v_commit, v_applied, v_read_index, v_read_round, v_term;
    uint64_t v_proven_index, v_proven_round, v_read_context;
    int v_has;          /* 1 when a local replica exists */
    int v_role, v_leader;
    uint32_t v_voters;  /* committed voting membership (routing hint) */
    int v_read_pending, v_read_done, v_proven_done;
    uint64_t v_stamp;   /* bumped by every publish that changed a field */
};

/* private fields: see the contract above. */
struct group_views {
    struct group_view_slot slot[GROUP_VIEW_NGROUPS];
    /* Leaf lock held by host_publish_view across its atomic stores only,
     * never across the getters; a reader that keeps finding v_seq odd waits
     * on it instead of spinning. */
    pthread_mutex_t view_mu;
    /* Sleepers (host_wait_applied, host_read_index) sleep under cv_mu, not
     * h->mu: their predicate is the published view, and the pump takes cv_mu
     * only to scan and count. */
    pthread_mutex_t cv_mu;
    pthread_cond_t applied_cv;
    /* Targeted wakeups: a sleeper registers the index it needs (or "any
     * change of this group's view") in a slot with its own condvar and the
     * pump signals only the slots whose predicate the fresh view satisfies.
     * applied_cv stays as the overflow path when every slot is taken. */
    struct host_waiter wait_slot[HOST_WAITERS];
    uint64_t waiter_active[HOST_WAITERS / 64]; /* cv_mu: occupied slots */
    uint32_t waiter_hint;
    uint32_t cv_overflow; /* waiters sleeping on applied_cv (under cv_mu) */
};

static inline struct group_view_slot *group_view_slot(struct group_views *gv,
                                                      uint8_t group)
{
    int i;
    for (i = 0; i < GROUP_VIEW_NGROUPS; i++) {
        if (gv->slot[i].group == group)
            return &gv->slot[i];
    }
    return NULL;
}

static inline int host_view_covers(const struct host_view *v, uint64_t want,
                            uint64_t round, uint64_t term)
{
    return v->has && v->role == EFS_RAFT_LEADER && v->proven_done &&
           v->proven_round >= round && v->term == term &&
           v->proven_index >= want && v->applied >= v->proven_index;
}

/* A fresh proof needs own apply, not another quorum request. Raw current-round
 * proof remains separate from the retained completion and is coherently read. */
/* A proof RPC returns an index, never KV rows. Its receiver must apply that
 * exact prefix before reading. Prefer an eligible retained proof; a current
 * fresh quorum is also sufficient even while the leader's own apply lags. */
static inline int host_view_quorum_index(const struct host_view *v, uint64_t want,
                                  uint64_t round, uint64_t term, uint64_t *idx)
{
    if (!v->has || v->role != EFS_RAFT_LEADER || v->term != term)
        return 0;
    if (v->proven_done && v->proven_round >= round && v->proven_index >= want) {
        *idx = v->proven_index;
        return 1;
    }
    if (v->read_done && v->read_round >= round && v->read_index >= want) {
        *idx = v->read_index;
        return 1;
    }
    return 0;
}

static inline int host_view_wait_apply(const struct host_view *v, uint64_t want,
                                uint64_t round, uint64_t term)
{
    return v->has && v->role == EFS_RAFT_LEADER && v->term == term &&
        ((v->proven_done && v->proven_round >= round &&
          v->proven_index >= want && v->applied < v->proven_index) ||
         (v->read_done && v->read_round >= round &&
          v->read_index >= want && v->applied < v->read_index));
}

/* The pump starts rounds (host_read_start_requested), so an idle round is
 * not a reason to wake this reader: only a proof, a covering round or a
 * change of role, term or replica is. */
static inline int host_read_wait_ready(const struct host_view *v,
                                 uint64_t want, uint64_t round, uint64_t term,
                                 int proof_only)
{
    uint64_t proof;
    if (!v->has || v->role != EFS_RAFT_LEADER || v->term != term)
        return 1;
    if (proof_only &&
        host_view_quorum_index(v, want, round, term, &proof))
        return 1;
    if (host_view_covers(v, want, round, term))
        return 1;
    return 0;
}

/* Freeze the first eligible quorum while it still owes own apply. Later
 * rounds must not turn this request's target into a moving write suffix.
 * Configuration, term, role and failed-storage invalidation still refuse. */
static inline int host_read_floor_ready(const struct host_view *v,
                                 struct host_read_floor *floor,
                                 uint64_t want,uint64_t round,uint64_t term,
                                 int proof_only)
{
    if(floor->context && v->read_context!=floor->context)return 1;
    if(!floor->context)floor->context=v->read_context;
    if (!floor->index && floor->context)
        (void)host_view_quorum_index(v,want,round,term,&floor->index);
    if (floor->index)
        return !v->has || v->role!=EFS_RAFT_LEADER || v->term!=term ||
               v->read_context!=floor->context || proof_only ||
               v->applied>=floor->index;
    return host_read_wait_ready(v,want,round,term,proof_only);
}


static inline void host_view_get(struct group_views *gv, uint8_t group,
                                 struct host_view *v)
{
    struct group_view_slot *g = group_view_slot(gv, group);

    uint64_t seq;
    unsigned odd = 0;
retry:
    seq = g ? __atomic_load_n(&g->v_seq, __ATOMIC_ACQUIRE) : 0;
    if (seq & 1) {
        if (++odd >= HOST_VIEW_TRIES) {
            pthread_mutex_lock(&gv->view_mu);
            pthread_mutex_unlock(&gv->view_mu);
            odd = 0;
        }
        goto retry;
    }
    memset(v, 0, sizeof(*v));
    v->leader = -1;
    if (!g)
        return;
    v->stamp = V_LOAD(g->v_stamp);
    v->has = V_LOAD(g->v_has);
    v->commit = V_LOAD(g->v_commit);
    v->applied = V_LOAD(g->v_applied);
    v->read_index = V_LOAD(g->v_read_index);
    v->read_round = V_LOAD(g->v_read_round);
    v->proven_round = V_LOAD(g->v_proven_round);
    v->proven_index = V_LOAD(g->v_proven_index);
    v->read_context = V_LOAD(g->v_read_context);
    v->term = V_LOAD(g->v_term);
    v->role = V_LOAD(g->v_role);
    v->leader = V_LOAD(g->v_leader);
    v->voters = V_LOAD(g->v_voters);
    v->read_pending = V_LOAD(g->v_read_pending);
    v->read_done = V_LOAD(g->v_read_done);
    v->proven_done = V_LOAD(g->v_proven_done);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (seq != __atomic_load_n(&g->v_seq, __ATOMIC_ACQUIRE))
        goto retry;
}
/* The wait bracket (see the contract). */
static inline void group_view_wait_lock(struct group_views *gv)
{
    pthread_mutex_lock(&gv->cv_mu);
}

static inline void group_view_wait_unlock(struct group_views *gv)
{
    pthread_mutex_unlock(&gv->cv_mu);
}

#endif
