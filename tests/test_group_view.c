/* Task V3: the published group view through its owner interface
 * (src/server/group_view.h).
 *
 * Links the real src/server/group_view.c and nothing else of the server. The
 * only symbols it takes from elsewhere are the twelve Raft core getters that
 * host_publish_view reads; this file defines them as stubs over one settable
 * core, so each case controls exactly what a publish sees. No case sleeps or
 * assumes a timing: the one thread race is bounded by a count.
 *
 * Cases (named; select by substring, see --list): unchanged publish leaves
 * the stamp alone; changed publish moves it and shows every field; an absent
 * replica clears has and the proof fields; an unknown group reads as absent;
 * a reader racing a publisher never sees a mixed publish (bounded by a
 * count); the predicates host_view_covers, host_view_quorum_index,
 * host_view_wait_apply and host_read_wait_ready; host_read_floor_ready for
 * the wait mode 3 (the floor freezes the first proof); the wait
 * bracket with a predicate that is already true; a publish before the
 * bracket is seen by the re-read, so no sleep (lost wakeup); a sleep whose
 * deadline has passed leaves no slot occupied and the floor as it was; a
 * wake with no sleeper changes nothing.
 *
 * A blocked mode-0 sleeper is counted by group_view_waiting and woken by a
 * publish that satisfies it (the main thread knows the sleeper is inside its
 * condition wait because it can take the wait lock only then).
 *
 * Not covered: the wake of a blocked sleeper in mode 1, the signal after the
 * wait lock is released, and the fall-back to the shared condition variable
 * when all HOST_WAITERS slots are busy. */
#include "group_view.h"
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct efs_raft {
    uint64_t commit, applied, term, read_index, read_round, read_context;
    uint64_t proven_round, proven_index;
    int role, leader, read_pending, read_done, proven_done;
    uint32_t voters;
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

#define GROUP 2
static struct group_views *new_views(void)
{
    struct group_views *gv = calloc(1, sizeof(*gv));

    assert(gv);
    group_view_init(gv);
    group_view_attach(gv, 0, 0);
    group_view_attach(gv, 1, GROUP);
    return gv;
}

static void free_views(struct group_views *gv)
{
    group_view_destroy(gv);
    free(gv);
}

static struct efs_raft leader_core(uint64_t k)
{
    struct efs_raft c = {.commit = k, .applied = k, .term = k, .read_index = k,
                         .read_round = k, .read_context = k, .proven_round = k,
                         .proven_index = k, .role = EFS_RAFT_LEADER, .leader = 1,
                         .read_pending = 0, .read_done = 1, .proven_done = 1,
                         .voters = 3};
    return c;
}

static struct host_view get(struct group_views *gv, uint8_t group)
{
    struct host_view v;

    host_view_get(gv, group, &v);
    return v;
}

static void test_unchanged_publish_keeps_stamp(void)
{
    struct group_views *gv = new_views();
    struct efs_raft c = leader_core(4);
    uint64_t s1;

    host_publish_view(gv, 1, &c);
    s1 = get(gv, GROUP).stamp;
    host_publish_view(gv, 1, &c);
    host_publish_view(gv, 1, &c);
    assert(get(gv, GROUP).stamp == s1);
    free_views(gv);
}

static void test_changed_publish_moves_stamp_and_shows_fields(void)
{
    struct group_views *gv = new_views();
    struct efs_raft c = leader_core(4);
    struct host_view v;
    uint64_t s1;

    host_publish_view(gv, 1, &c);
    v = get(gv, GROUP);
    s1 = v.stamp;
    assert(v.has && v.role == EFS_RAFT_LEADER && v.leader == 1 && v.voters == 3);
    assert(v.commit == 4 && v.applied == 4 && v.term == 4 && v.read_index == 4);
    assert(v.read_round == 4 && v.proven_round == 4 && v.proven_index == 4);
    assert(v.read_context == 4 && v.read_done && v.proven_done && !v.read_pending);
    c.applied = 5;
    host_publish_view(gv, 1, &c);
    v = get(gv, GROUP);
    assert(v.stamp == s1 + 1 && v.applied == 5 && v.commit == 4);
    free_views(gv);
}

static void test_absent_replica_clears_has_and_proof(void)
{
    struct group_views *gv = new_views();
    struct efs_raft c = leader_core(4);
    struct host_view v;
    uint64_t s1;

    host_publish_view(gv, 1, &c);
    s1 = get(gv, GROUP).stamp;
    host_publish_view(gv, 1, NULL);
    v = get(gv, GROUP);
    assert(!v.has && !v.read_done && !v.proven_done);
    assert(v.proven_round == 0 && v.proven_index == 0 && v.read_context == 0);
    assert(v.stamp == s1 + 1); /* has was set: waiters see the change */
    host_publish_view(gv, 1, NULL);
    assert(get(gv, GROUP).stamp == s1 + 1); /* already absent: no change */
    free_views(gv);
}

static void test_unknown_group_reads_as_absent(void)
{
    struct group_views *gv = new_views();
    struct host_view v = get(gv, 6);

    assert(!v.has && v.leader == -1 && v.stamp == 0 && v.commit == 0);
    v = get(gv, GROUP); /* attached, never published */
    assert(!v.has);
    free_views(gv);
}

#define PUBLISHES 20000
#define READS 200000
static struct group_views *race_gv;

static void *race_publisher(void *arg)
{
    struct efs_raft c;

    (void)arg;
    for (uint64_t k = 1; k <= PUBLISHES; k++) {
        c = leader_core(k);
        host_publish_view(race_gv, 1, &c);
    }
    return NULL;
}

static void test_reader_never_sees_a_mixed_publish(void)
{
    pthread_t t;
    uint64_t last = 0;

    race_gv = new_views();
    assert(pthread_create(&t, NULL, race_publisher, NULL) == 0);
    for (int i = 0; i < READS; i++) {
        struct host_view v = get(race_gv, GROUP);

        if (!v.has)
            continue;
        assert(v.term == v.read_round && v.term == v.proven_round);
        assert(v.term == v.commit && v.term == v.applied && v.term == v.read_context);
        assert(v.term == v.read_index && v.term == v.proven_index);
        assert(v.term >= last); /* a reader never goes back */
        last = v.term;
    }
    pthread_join(t, NULL);
    assert(get(race_gv, GROUP).term == PUBLISHES);
    free_views(race_gv);
}

static void test_predicates(void)
{
    struct host_view v = {.has = 1, .role = EFS_RAFT_LEADER, .term = 3,
                          .proven_done = 1, .proven_round = 2, .proven_index = 10,
                          .applied = 10};
    uint64_t idx = 0;

    assert(host_view_covers(&v, 8, 2, 3));
    assert(!host_view_covers(&v, 11, 2, 3));   /* proof below the want */
    assert(!host_view_covers(&v, 8, 3, 3));    /* proof of an older round */
    assert(!host_view_covers(&v, 8, 2, 4));    /* other term */
    v.applied = 9;
    assert(!host_view_covers(&v, 8, 2, 3));    /* own apply behind the proof */
    assert(host_view_wait_apply(&v, 8, 2, 3)); /* waiting for own apply */
    assert(host_view_quorum_index(&v, 8, 2, 3, &idx) && idx == 10);
    v.proven_done = 0;
    v.read_done = 1; v.read_round = 2; v.read_index = 12;
    assert(host_view_quorum_index(&v, 8, 2, 3, &idx) && idx == 12);
    v.has = 0;
    assert(!host_view_quorum_index(&v, 8, 2, 3, &idx));
    assert(host_read_wait_ready(&v, 8, 2, 3, 0)); /* no replica: re-check */
    v.has = 1; v.role = 0;
    assert(host_read_wait_ready(&v, 8, 2, 3, 0)); /* not leader: re-check */
}

static void test_floor_ready_mode_3(void)
{
    struct host_view v = {.has = 1, .role = EFS_RAFT_LEADER, .term = 3,
                          .read_context = 7};
    struct host_read_floor f = {0, 0};

    /* idle round: the pump starts rounds, so an idle round is not ready */
    assert(!host_read_floor_ready(&v, &f, 5, 1, 3, 0));
    assert(f.context == 7 && f.index == 0);
    /* a proof that owes own apply freezes the floor at its index */
    v.proven_done = 1; v.proven_round = 1; v.proven_index = 6; v.applied = 4;
    f = (struct host_read_floor){0, 0};
    assert(!host_read_floor_ready(&v, &f, 5, 1, 3, 0));
    assert(f.index == 6);
    v.proven_index = 9;                      /* a later round does not move it */
    assert(!host_read_floor_ready(&v, &f, 5, 1, 3, 0));
    assert(f.index == 6);
    v.applied = 6;
    assert(host_read_floor_ready(&v, &f, 5, 1, 3, 0));
    v.applied = 4;
    assert(host_read_floor_ready(&v, &f, 5, 1, 3, 1)); /* proof_only */
    v.read_context = 8;                      /* another context: refuse */
    assert(host_read_floor_ready(&v, &f, 5, 1, 3, 0));
}

static void test_bracket_true_predicate_returns_at_once(void)
{
    struct group_views *gv = new_views();
    struct efs_raft c = leader_core(4);
    struct host_view v;

    host_publish_view(gv, 1, &c);
    for (int i = 0; i < 2; i++) { /* a second pass proves the first unlocked */
        group_view_wait_lock(gv);
        host_view_get(gv, GROUP, &v);
        if (!(v.has && v.applied >= 4))
            abort();               /* the predicate holds: no host_waiter_sleep */
        group_view_wait_unlock(gv);
    }
    assert(!group_view_waiting(gv));
    free_views(gv);
}

static void test_publish_before_bracket_is_seen_no_sleep(void)
{
    struct group_views *gv = new_views();
    struct efs_raft c = leader_core(4);
    struct host_view v;
    uint64_t seen;

    host_publish_view(gv, 1, &c);
    seen = get(gv, GROUP).stamp;      /* the waiter decided to wait on this */
    c.applied = 9;
    host_publish_view(gv, 1, &c);     /* the publish lands before the lock */
    group_view_wait_lock(gv);
    host_view_get(gv, GROUP, &v);
    assert(v.stamp != seen);          /* mode 1 predicate: already moved */
    group_view_wait_unlock(gv);
    free_views(gv);
}

static void test_passed_deadline_sleep_leaves_no_slot(void)
{
    struct group_views *gv = new_views();
    struct timespec past = {0, 0};
    struct host_read_floor f = {3, 7};

    for (int any = 0; any <= 3; any++) {
        group_view_wait_lock(gv);
        host_waiter_sleep(gv, GROUP, any, 5, 1, &past, 1, 1, 0, any >= 2 ? &f : NULL);
        group_view_wait_unlock(gv);
        assert(!group_view_waiting(gv));
        assert(f.index == 3 && f.context == 7); /* floor copied in and back */
    }
    free_views(gv);
}

static void test_wake_without_sleeper_changes_nothing(void)
{
    struct group_views *gv = new_views();
    struct efs_raft c = leader_core(4);
    struct host_view a, b;

    host_publish_view(gv, 1, &c);
    a = get(gv, GROUP);
    host_waiters_wake(gv);
    b = get(gv, GROUP);
    assert(a.stamp == b.stamp && a.commit == b.commit && a.applied == b.applied && a.has == b.has);
    assert(!group_view_waiting(gv));
    free_views(gv);
}

struct sleeper {
    struct group_views *gv;
    sem_t locked;
    struct timespec end;
};

static void *sleeper_thread(void *p)
{
    struct sleeper *s = p;

    group_view_wait_lock(s->gv);
    sem_post(&s->locked);
    host_waiter_sleep(s->gv, GROUP, 0, 9, 0, &s->end, 0, 0, 0, NULL); /* mode 0: until applied >= 9 */
    group_view_wait_unlock(s->gv);
    return NULL;
}

static void test_blocked_sleeper_is_counted_and_woken(void)
{
    struct group_views *gv = new_views();
    struct efs_raft c = leader_core(4);
    struct sleeper s = {.gv = gv};
    struct timespec now;
    pthread_t t;

    host_publish_view(gv, 1, &c);
    assert(!sem_init(&s.locked, 0, 0));
    clock_gettime(CLOCK_REALTIME, &s.end);
    s.end.tv_sec += 30; /* a failure bound for the sleeper, never reached when the wake works */
    assert(!pthread_create(&t, NULL, sleeper_thread, &s));
    sem_wait(&s.locked);
    /* The sleeper holds the wait lock until its condition wait releases it, so
     * this lock is granted only once the sleeper is registered and asleep. */
    group_view_wait_lock(gv);
    group_view_wait_unlock(gv);
    assert(group_view_waiting(gv) == 1); /* the reaper's yield test sees it */
    c = leader_core(9);
    host_publish_view(gv, 1, &c);
    host_waiters_wake(gv);
    assert(!pthread_join(t, NULL));
    clock_gettime(CLOCK_REALTIME, &now);
    assert(now.tv_sec < s.end.tv_sec - 10); /* woken by the publish, not by its bound */
    assert(!group_view_waiting(gv));
    sem_destroy(&s.locked);
    free_views(gv);
}

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
    RUN(test_unchanged_publish_keeps_stamp);
    RUN(test_changed_publish_moves_stamp_and_shows_fields);
    RUN(test_absent_replica_clears_has_and_proof);
    RUN(test_unknown_group_reads_as_absent);
    RUN(test_reader_never_sees_a_mixed_publish);
    RUN(test_predicates);
    RUN(test_floor_ready_mode_3);
    RUN(test_bracket_true_predicate_returns_at_once);
    RUN(test_publish_before_bracket_is_seen_no_sleep);
    RUN(test_passed_deadline_sleep_leaves_no_slot);
    RUN(test_wake_without_sleeper_changes_nothing);
    RUN(test_blocked_sleeper_is_counted_and_woken);
    if (g_list)
        return 0;
    for (int i = 1; i < argc; i++)
        if (!g_used[i]) {
            fprintf(stderr, "%s: no case matches '%s' (see --list)\n", argv[0], argv[i]);
            bad = 1;
        }
    if (bad)
        return 2;
    puts("group_view interface: unchanged/changed publish, absent replica, unknown group, "
         "coherent reader race, predicates, floor modes, wait bracket, lost wakeup, "
         "passed deadline, idle wake PASS");
    return 0;
}
