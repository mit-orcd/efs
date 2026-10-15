/* durable_cohort.h: group commit, failure ownership, next-set arrivals and
 * the bounded gather window, with an injected barrier. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "../src/server/durable_cohort.h"

static atomic_int barrier_calls, fail_next, hold_barrier;
static atomic_int in_barrier;

static int test_barrier(int fd)
{
    (void)fd;
    atomic_fetch_add(&barrier_calls, 1);
    atomic_store(&in_barrier, 1);
    while (atomic_load(&hold_barrier))
        usleep(200);
    usleep(500); /* a barrier takes time; arrivals meanwhile form the next set */
    atomic_store(&in_barrier, 0);
    return atomic_exchange(&fail_next, 0) ? -1 : 0;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static struct durable_cohort c = DURABLE_COHORT_INIT;

struct member { int delay_us, rc, begun; };
static void *member(void *arg)
{
    struct member *m = arg;
    if (!m->begun)
        durable_cohort_begin(&c);
    usleep((unsigned)m->delay_us);          /* the O_DIRECT write */
    m->rc = durable_cohort_join(&c);
    return NULL;
}

static void reset(unsigned gather_us)
{
    c.gather_us = gather_us;
    c.barriers = c.members = c.failures = 0;
    atomic_store(&barrier_calls, 0);
    assert(!c.head && !c.busy && !c.writing);
}

int main(void)
{
    c.barrier = test_barrier;

    /* 1. Concurrent group commit: every member ACKs, barriers are shared. */
    reset(0);
    enum { N = 64 };
    pthread_t th[N];
    struct member m[N];
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < N; i++) {
            m[i] = (struct member){ .delay_us = (i * 37) % 900 };
            pthread_create(&th[i], NULL, member, &m[i]);
        }
        for (int i = 0; i < N; i++) {
            pthread_join(th[i], NULL);
            assert(m[i].rc == EFS_OK);
        }
    }
    assert(c.members == 4 * N && c.barriers < c.members && c.failures == 0);
    assert(c.barriers == (uint64_t)atomic_load(&barrier_calls));
    printf("group commit: %llu members in %llu barriers\n",
           (unsigned long long)c.members, (unsigned long long)c.barriers);

    /* 2. A failed barrier fails every member it covered, and only those.
     * Eight members begin first, so a generous gather window collects all
     * of them into one cohort; that barrier fails. */
    reset(500000);
    for (int i = 0; i < 8; i++)
        durable_cohort_begin(&c);
    atomic_store(&fail_next, 1);
    for (int i = 0; i < 8; i++) {
        m[i] = (struct member){ .delay_us = 1000 + i * 2000, .begun = 1 };
        pthread_create(&th[i], NULL, member, &m[i]);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
        assert(m[i].rc == EFS_ERR_IO);
    }
    assert(c.barriers == 1 && c.members == 8 && c.failures == 1);
    /* The next cohort succeeds. */
    durable_cohort_begin(&c);
    assert(durable_cohort_join(&c) == EFS_OK && c.barriers == 2 && c.failures == 1);

    /* 3. Arrivals while a barrier is running belong to the next set: a
     * member joining during a failing barrier must not inherit its failure
     * (nor be ACKed by a barrier that started before its write finished). */
    reset(0);
    atomic_store(&hold_barrier, 1);
    atomic_store(&fail_next, 1);
    m[0] = (struct member){0};
    pthread_create(&th[0], NULL, member, &m[0]);
    while (!atomic_load(&in_barrier))
        usleep(100);
    m[1] = (struct member){0};
    pthread_create(&th[1], NULL, member, &m[1]);
    usleep(20000);                          /* B is queued behind A's barrier */
    atomic_store(&hold_barrier, 0);
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    assert(m[0].rc == EFS_ERR_IO && m[1].rc == EFS_OK);
    assert(c.barriers == 2 && c.members == 2 && c.failures == 1);

    /* 4. Gather closes as soon as every begun member has joined... */
    reset(1000000);                         /* 1 s window */
    for (int i = 0; i < 4; i++)
        durable_cohort_begin(&c);
    double t0 = now_ms();
    for (int i = 0; i < 4; i++) {
        m[i] = (struct member){ .delay_us = i * 10000, .begun = 1 };
        pthread_create(&th[i], NULL, member, &m[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        assert(m[i].rc == EFS_OK);
    }
    double took = now_ms() - t0;
    assert(c.barriers == 1 && c.members == 4 && took < 500);

    /* ...and is bounded when a begun member never joins; abort releases it. */
    reset(50000);                           /* 50 ms window */
    durable_cohort_begin(&c);               /* never joins */
    durable_cohort_begin(&c);
    t0 = now_ms();
    assert(durable_cohort_join(&c) == EFS_OK);
    took = now_ms() - t0;
    assert(took >= 45 && took < 1000 && c.barriers == 1 && c.writing == 1);
    durable_cohort_abort(&c);
    assert(c.writing == 0);

    /* 5. An abort wakes a gathering leader waiting only on that writer. */
    reset(5000000);                         /* 5 s window */
    durable_cohort_begin(&c);               /* will abort */
    durable_cohort_begin(&c);
    m[0] = (struct member){ .begun = 1 };
    t0 = now_ms();
    pthread_create(&th[0], NULL, member, &m[0]);
    usleep(20000);
    durable_cohort_abort(&c);
    pthread_join(th[0], NULL);
    took = now_ms() - t0;
    assert(m[0].rc == EFS_OK && took < 1000 && c.writing == 0);

    puts("durable cohort: group commit, failure covers only its cohort, next-set "
         "arrivals, early gather close, bounded gather and abort PASS");
    return 0;
}
