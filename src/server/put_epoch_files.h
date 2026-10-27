#ifndef EFS_PUT_EPOCH_FILES_H
#define EFS_PUT_EPOCH_FILES_H
#include "efs/common.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

/* Runtime ownership only; no new store or wire encoding. At most 256 retained
 * descriptors daemon-wide (or 1/4 of its soft limit), not per storage root.
 * Callers disable cancellation while owning or waiting for these pins.
 * Original writer descriptors are closed and checked before PUT is ACKed. */
#define PUT_EPOCH_FILES 128u
#define PUT_EPOCH_WORKERS 8u
struct put_epoch_file { int fd; void *owner; char path[EFS_MAX_PATH]; };
static pthread_mutex_t put_epoch_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t put_epoch_cv=PTHREAD_COND_INITIALIZER;
static pthread_once_t put_epoch_once=PTHREAD_ONCE_INIT;
static unsigned put_epoch_owned,put_epoch_limit=256;
static void put_epoch_init(void)
{
    struct rlimit r;
    if(!getrlimit(RLIMIT_NOFILE,&r) && r.rlim_cur!=RLIM_INFINITY && r.rlim_cur/4<put_epoch_limit)
        put_epoch_limit=r.rlim_cur/4 ? (unsigned)(r.rlim_cur/4) : 1;
}
static void put_epoch_uncharge(void)
{
    pthread_mutex_lock(&put_epoch_mu);
    assert(put_epoch_owned);put_epoch_owned--;
    pthread_cond_broadcast(&put_epoch_cv);pthread_mutex_unlock(&put_epoch_mu);
}
static int put_epoch_pin(int fd)
{
    pthread_once(&put_epoch_once,put_epoch_init);
    pthread_mutex_lock(&put_epoch_mu);
    while(put_epoch_owned>=put_epoch_limit)pthread_cond_wait(&put_epoch_cv,&put_epoch_mu);
    put_epoch_owned++;pthread_mutex_unlock(&put_epoch_mu);
    int pin;
    do{pin=fcntl(fd,F_DUPFD_CLOEXEC,0);}while(pin<0 && errno==EINTR);
    if(pin<0)put_epoch_uncharge();
    return pin;
}
static int put_epoch_close(int fd)
{
    int rc=close(fd); /* never retry close, including EINTR */
    put_epoch_uncharge();return rc ? EFS_ERR_IO : EFS_OK;
}
struct put_epoch_work {
    struct put_epoch_file *files;
    unsigned count,next;
    int error;
    int (*sync)(const struct put_epoch_file *);
};
static void *put_epoch_worker(void *arg)
{
    struct put_epoch_work *w=arg;
    for(;;){
        unsigned i=__atomic_fetch_add(&w->next,1,__ATOMIC_RELAXED);
        if(i>=w->count)break;
        int rc=w->sync(&w->files[i]);
        int closed=put_epoch_close(w->files[i].fd);
        if(rc!=EFS_OK || closed!=EFS_OK)__atomic_store_n(&w->error,EFS_ERR_IO,__ATOMIC_RELAXED);
    }
    return NULL;
}
/* A frozen cohort owns its pins through all file/name barriers. Every pin is
 * closed on success or failure; new arrivals belong to the next epoch. */
static int put_epoch_sync(struct put_epoch_file *files,unsigned count,
                           int (*sync)(const struct put_epoch_file *))
{
    struct put_epoch_work w={.files=files,.count=count,.sync=sync};
    pthread_t threads[PUT_EPOCH_WORKERS-1];unsigned started=0;
    for(unsigned i=1;i<count && i<PUT_EPOCH_WORKERS;i++)
        if(!pthread_create(&threads[started],NULL,put_epoch_worker,&w))started++;
    put_epoch_worker(&w);
    for(unsigned i=0;i<started;i++)pthread_join(threads[i],NULL);
    return w.error;
}
#endif
