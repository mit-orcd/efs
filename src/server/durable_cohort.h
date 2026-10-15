#ifndef EFS_DURABLE_COHORT_H
#define EFS_DURABLE_COHORT_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include "efs/common.h"

/* Grouped durability for fragment files newly created on one storage root
 * (docs/status/intel-cohort-durability-20261015.md). Each member has created
 * its file with O_CREAT|O_EXCL, written it with O_DIRECT and closed it, but
 * issued no file barrier. One barrier (syncfs of the root on XFS: a log force
 * whose first write carries a cache flush) then makes every member's data,
 * create, directory entry and extent conversion durable together.
 *
 * As with directory_sync_group: the leader detaches the covered set before
 * starting the barrier, arrivals during I/O belong to the next set, and a
 * failed barrier fails every member it covered (no partial ACK). Before
 * detaching, the leader may wait up to gather_us while members that called
 * durable_cohort_begin have not yet joined, so a lone PUT never waits the
 * full window. Every begin must be matched by exactly one join or abort. */
struct durable_cohort_waiter {
    struct durable_cohort_waiter *next;
    int done, rc;
};
struct durable_cohort {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct durable_cohort_waiter *head;
    int busy;
    unsigned writing;   /* begun, not yet joined or aborted */
    unsigned gather_us;
    int fd;             /* root descriptor passed to barrier */
    int (*barrier)(int fd);
    uint64_t barriers, members, failures;
};
#define DURABLE_COHORT_INIT { .mu = PTHREAD_MUTEX_INITIALIZER, \
                              .cv = PTHREAD_COND_INITIALIZER, .fd = -1 }

static inline void durable_cohort_begin(struct durable_cohort *c)
{
    pthread_mutex_lock(&c->mu);
    c->writing++;
    pthread_mutex_unlock(&c->mu);
}

/* The member will not join (its write or close failed). */
static inline void durable_cohort_abort(struct durable_cohort *c)
{
    pthread_mutex_lock(&c->mu);
    if (c->writing)
        c->writing--;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

/* EFS_OK once a successful barrier that started after this call covers the
 * caller; EFS_ERR_IO if the covering barrier failed. */
static inline int durable_cohort_join(struct durable_cohort *c)
{
    struct durable_cohort_waiter mine = {0};
    int cancellation;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancellation);
    pthread_mutex_lock(&c->mu);
    if (c->writing)
        c->writing--;
    mine.next = c->head;
    c->head = &mine;
    pthread_cond_broadcast(&c->cv); /* a gathering leader may be waiting */
    while (!mine.done) {
        if (c->busy) {
            pthread_cond_wait(&c->cv, &c->mu);
            continue;
        }
        c->busy = 1;
        if (c->writing && c->gather_us) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += (long)c->gather_us * 1000;
            until.tv_sec += until.tv_nsec / 1000000000;
            until.tv_nsec %= 1000000000;
            while (c->writing &&
                   pthread_cond_timedwait(&c->cv, &c->mu, &until) != ETIMEDOUT)
                ;
        }
        struct durable_cohort_waiter *batch = c->head;
        c->head = NULL;
        pthread_mutex_unlock(&c->mu);
        int rc = c->barrier(c->fd) == 0 ? EFS_OK : EFS_ERR_IO;
        pthread_mutex_lock(&c->mu);
        uint64_t n = 0;
        while (batch) {
            struct durable_cohort_waiter *next = batch->next;
            batch->rc = rc;
            batch->done = 1;
            batch = next;
            n++;
        }
        c->barriers++;
        c->members += n;
        if (rc != EFS_OK)
            c->failures++;
        c->busy = 0;
        pthread_cond_broadcast(&c->cv);
    }
    int rc = mine.rc;
    pthread_mutex_unlock(&c->mu);
    pthread_setcancelstate(cancellation, NULL);
    return rc;
}
#endif
