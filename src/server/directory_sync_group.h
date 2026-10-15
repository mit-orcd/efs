#ifndef EFS_DIRECTORY_SYNC_GROUP_H
#define EFS_DIRECTORY_SYNC_GROUP_H
#include <pthread.h>
#include <sys/stat.h>
#include "efs/common.h"

/* Only directory barriers may be shared. Each caller has finished its
 * namespace mutation and retains its descriptor until this function returns.
 * The leader detaches the covered set BEFORE starting the barrier. Arrivals
 * during I/O belong to the next set, even on success. No persisted-success
 * cache, inode lifetime guess, delay, or per-request heap allocation. */
struct directory_sync_waiter {
    struct directory_sync_waiter *next;
    dev_t dev;
    ino_t ino;
    int fd, done, rc;
    int (*sync)(void *, int);
    void *context;
};
#define DIRECTORY_SYNC_BUCKETS 64u
struct directory_sync_group {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct directory_sync_waiter *head[DIRECTORY_SYNC_BUCKETS];
    unsigned char busy[DIRECTORY_SYNC_BUCKETS];
};
#define DIRECTORY_SYNC_GROUP_INIT { .mu=PTHREAD_MUTEX_INITIALIZER, .cv=PTHREAD_COND_INITIALIZER }

static int directory_sync_group_run(struct directory_sync_group *g, int fd,
                                   int (*sync)(void *, int), void *context)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode)) return EFS_ERR_IO;
    struct directory_sync_waiter mine = {
        .dev=st.st_dev, .ino=st.st_ino, .fd=fd, .sync=sync, .context=context
    };
    unsigned bucket=((uint64_t)st.st_ino ^ (uint64_t)st.st_dev) % DIRECTORY_SYNC_BUCKETS;
    /* A cancelled caller cannot leave a stack waiter or descriptor behind. */
    int cancellation;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancellation);
    pthread_mutex_lock(&g->mu);
    struct directory_sync_waiter **tail=&g->head[bucket];
    while (*tail) tail=&(*tail)->next;
    *tail=&mine;
    while (!mine.done) {
        if (g->busy[bucket]) {
            pthread_cond_wait(&g->cv, &g->mu);
            continue;
        }
        struct directory_sync_waiter *first=g->head[bucket], *batch=NULL;
        struct directory_sync_waiter **link=&g->head[bucket];
        while (*link) {
            struct directory_sync_waiter *w=*link;
            if (w->dev==first->dev && w->ino==first->ino &&
                w->sync==first->sync && w->context==first->context) {
                *link=w->next; w->next=batch; batch=w;
            } else link=&w->next;
        }
        g->busy[bucket]=1;
        pthread_mutex_unlock(&g->mu);
        int rc=first->sync(first->context, first->fd);
        pthread_mutex_lock(&g->mu);
        while (batch) {
            struct directory_sync_waiter *next=batch->next;
            batch->rc=rc; batch->done=1; batch=next;
        }
        g->busy[bucket]=0;
        pthread_cond_broadcast(&g->cv);
    }
    int rc=mine.rc;
    pthread_mutex_unlock(&g->mu);
    pthread_setcancelstate(cancellation, NULL);
    return rc;
}
#endif
