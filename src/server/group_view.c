#include "group_view.h"

/* The published group view and its sleepers: see group_view.h for the
 * contract. The bodies below moved from raft_host.c unchanged except for how
 * they reach the state. */

void group_view_init(struct group_views *gv)
{
    uint32_t k;

    pthread_mutex_init(&gv->view_mu, NULL);
    pthread_mutex_init(&gv->cv_mu, NULL);
    pthread_cond_init(&gv->applied_cv, NULL);
    for (k = 0; k < HOST_WAITERS; k++)
        pthread_cond_init(&gv->wait_slot[k].cv, NULL);
}

void group_view_destroy(struct group_views *gv)
{
    uint32_t k;

    pthread_mutex_destroy(&gv->view_mu);
    pthread_cond_destroy(&gv->applied_cv);
    for (k = 0; k < HOST_WAITERS; k++)
        pthread_cond_destroy(&gv->wait_slot[k].cv);
    pthread_mutex_destroy(&gv->cv_mu);
}

void group_view_attach(struct group_views *gv, int gi, uint8_t group)
{
    gv->slot[gi].group = group;
    V_STORE(gv->slot[gi].v_has, 0);
}

/* 1 when a sleeper slot is occupied. */
int group_view_waiting(struct group_views *gv)
{
    int waiting=0;
    pthread_mutex_lock(&gv->cv_mu);
    for(unsigned k=0;k<HOST_WAITERS;k++)
        if(gv->wait_slot[k].active){waiting=1;break;}
    pthread_mutex_unlock(&gv->cv_mu);
    return waiting;
}

/* Publish one group's replica state for lock-free readers. The caller holds h->mu and hands over
 * the group's core r (NULL: no local replica). */
void host_publish_view(struct group_views *gv, int gi, struct efs_raft *r)
{
    struct group_view_slot *g = &gv->slot[gi];

    uint64_t commit, applied, term, ridx, round, proven_index, proven_round;
    uint32_t voters;
    int role, leader, rpend, rdone, proven_done, changed;

    if (!r) {
        pthread_mutex_lock(&gv->view_mu);
        __atomic_add_fetch(&g->v_seq, 1, __ATOMIC_SEQ_CST);
        if (V_LOAD(g->v_has))
            V_STORE(g->v_stamp, V_LOAD(g->v_stamp) + 1);
        V_STORE(g->v_has, 0);
        V_STORE(g->v_read_done, 0);
        V_STORE(g->v_proven_done, 0);
        V_STORE(g->v_proven_round, 0);
        V_STORE(g->v_proven_index, 0);
        V_STORE(g->v_read_context, 0);
        __atomic_add_fetch(&g->v_seq, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&gv->view_mu);
        return;
    }
    commit = efs_raft_commit(r);
    applied = efs_raft_applied(r);
    term = efs_raft_term(r);
    role = (int)efs_raft_role(r);
    leader = efs_raft_leader(r);
    voters = efs_raft_voters(r);
    ridx = efs_raft_read_index(r);
    round = efs_raft_read_round(r);
    rpend = efs_raft_read_pending(r);
    rdone = efs_raft_read_quorum(r);
    proven_done = efs_raft_read_proven(r, &proven_round, &proven_index);
    uint64_t read_context=efs_raft_read_context(r);
    changed = V_LOAD(g->v_read_context)!=read_context || !V_LOAD(g->v_has) || V_LOAD(g->v_commit) != commit ||
              V_LOAD(g->v_applied) != applied || V_LOAD(g->v_term) != term ||
              V_LOAD(g->v_role) != role || V_LOAD(g->v_leader) != leader ||
              V_LOAD(g->v_voters) != voters ||
              V_LOAD(g->v_read_index) != ridx || V_LOAD(g->v_read_round) != round ||
              V_LOAD(g->v_read_pending) != rpend ||
              V_LOAD(g->v_read_done) != rdone ||
              V_LOAD(g->v_proven_done) != proven_done ||
              V_LOAD(g->v_proven_round) != proven_round ||
              V_LOAD(g->v_proven_index) != proven_index;
    /* Core getters can wait for the shared disk-log mutex. Keep the old
     * coherent view readable while collecting state under h->mu; mark the
     * sequence odd only for the bounded atomic copy. Unchanged state needs
     * neither a sequence write nor cache-line invalidation. */
    if (!changed)
        return;
    pthread_mutex_lock(&gv->view_mu);
    __atomic_add_fetch(&g->v_seq, 1, __ATOMIC_SEQ_CST);
    V_STORE(g->v_read_context, read_context);
    V_STORE(g->v_commit, commit);
    V_STORE(g->v_applied, applied);
    V_STORE(g->v_term, term);
    V_STORE(g->v_role, role);
    V_STORE(g->v_leader, leader);
    V_STORE(g->v_voters, voters);
    V_STORE(g->v_read_index, ridx);
    V_STORE(g->v_read_round, round);
    V_STORE(g->v_read_pending, rpend);
    V_STORE(g->v_read_done, rdone);
    V_STORE(g->v_proven_done, proven_done);
    V_STORE(g->v_proven_round, proven_round);
    V_STORE(g->v_proven_index, proven_index);
    V_STORE(g->v_has, 1);
    /* Stamp last: a waiter that read the new stamp has the new fields. */
    if (changed)
        V_STORE(g->v_stamp, V_LOAD(g->v_stamp) + 1);
    __atomic_add_fetch(&g->v_seq, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&gv->view_mu);
}

/* Sleep until the pump wakes this waiter or `end` passes. cv_mu HELD by
 * the caller, who has just re-read the view under it and found its
 * predicate false (that order is what rules out a lost wakeup). any=0:
 * wake when the group's applied >= idx (or the replica goes away); any=1:
 * wake when the group's view stamp moves past `seen`. Falls back to the
 * shared applied_cv when all slots are busy. any=3 wakes for actionable
 * fresh proof progress of a reader whose rounds the pump starts (never woken
 * just because no round is pending); there is no any=2 (removed with the direct mode). Returns with cv_mu held. */
void host_waiter_sleep(struct group_views *gv, uint8_t group, int any,
                              uint64_t idx, uint64_t seen,
                              const struct timespec *end, uint64_t round, uint64_t term,
                              int proof_only, struct host_read_floor *floor)
{
    uint32_t i, n = HOST_WAITERS;
    struct host_waiter *w = NULL;

    for (i = 0; i < n; i++) {
        uint32_t k = (gv->waiter_hint + i) % n;
        if (!gv->wait_slot[k].active) {
            w = &gv->wait_slot[k];
            gv->waiter_hint = k + 1;
            break;
        }
    }
    if (!w) {
        gv->cv_overflow++;
        pthread_cond_timedwait(&gv->applied_cv, &gv->cv_mu, end);
        gv->cv_overflow--;
        return;
    }
    w->group = group;
    w->any = (uint8_t)any;
    w->idx = idx;
    w->seen = seen;
    w->round = round;
    w->term = term;
    w->proof_only = (uint8_t)proof_only;
    w->floor = floor ? *floor : (struct host_read_floor){0};
    w->notified = 0;
    w->active = 1;
    uint32_t slot = (uint32_t)(w - gv->wait_slot);
    gv->waiter_active[slot / 64] |= UINT64_C(1) << (slot % 64);
    pthread_cond_timedwait(&w->cv, &gv->cv_mu, end);
    w->active = 0;
    gv->waiter_active[slot / 64] &= ~(UINT64_C(1) << (slot % 64));
    if(floor)*floor=w->floor;
}

/* Pump side: after publishing every hosted group's view, wake the waiters
 * whose predicate that view satisfies, and only those.
 *
 * The scan decides under cv_mu (notified=1, floors captured) but signals
 * after unlocking. glibc has no wait morphing: a waiter signalled under
 * cv_mu wakes only to block on cv_mu until the scan ends, and that
 * re-acquisition was 45 % of leader lock waits (orcd-mini profile on
 * f4a373b13). No wakeup is lost: each chosen waiter registered and parked
 * under cv_mu before this scan took it, so it is either still blocked on
 * its cv (the late signal reaches it) or has already left by timeout and
 * re-checks the view itself. A slot freed and reused before the late
 * signal gets one spurious wakeup; every sleeper re-checks its predicate
 * in a loop, and slot condvars live until host teardown. */
void host_waiters_wake(struct group_views *gv)
{
    struct host_view view[GROUP_VIEW_NGROUPS];
    uint16_t sig[HOST_WAITERS];
    uint32_t nsig = 0, overflow;
    uint8_t loaded = 0;

    pthread_mutex_lock(&gv->cv_mu);
    /* Registration/removal and this snapshot share cv_mu. Scan occupied
     * slots only; notified readers still need their first proof captured. */
    for (uint32_t word = 0; word < HOST_WAITERS / 64; word++) {
      uint64_t active = gv->waiter_active[word];
      while (active) {
        uint32_t k = word * 64 + (uint32_t)__builtin_ctzll(active);
        active &= active - 1;
        struct host_waiter *w = &gv->wait_slot[k];
        struct group_view_slot *g;
        int wake;

        if (!w->active || (w->notified &&
            (w->any!=3 || w->floor.index || !w->floor.context)))
            continue;
        g = group_view_slot(gv, w->group);
        if (!g || !V_LOAD(g->v_has))
            wake = 1;
        else if (w->any == 3) {
            unsigned gi = (unsigned)(g - gv->slot);
            if (!(loaded & (1u << gi))) {
                host_view_get(gv, w->group, &view[gi]);
                loaded |= (uint8_t)(1u << gi);
            }
            wake = host_read_floor_ready(&view[gi], &w->floor, w->idx, w->round, w->term,
                                         w->proof_only);
        } else if (w->any)
            wake = V_LOAD(g->v_stamp) != w->seen;
        else
            wake = V_LOAD(g->v_applied) >= w->idx;
        if (wake && !w->notified) {
            w->notified = 1;
            sig[nsig++] = (uint16_t)k;
        }
    }
    }
    overflow = gv->cv_overflow;
    pthread_mutex_unlock(&gv->cv_mu);
    for (uint32_t i = 0; i < nsig; i++)
        pthread_cond_signal(&gv->wait_slot[sig[i]].cv);
    if (overflow)
        pthread_cond_broadcast(&gv->applied_cv);
}
