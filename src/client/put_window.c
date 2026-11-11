#include "put_window.h"
#include "client_internal.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* Moved unchanged from write.c (the only edit: put_win_wait reads the
 * clock through the private put_win_clock_ms, a copy of report_clock_ms). */
static uint64_t put_win_clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* PUT windows. A flusher marks a dcache entry clean before its PUT and
 * records the chunk in the dirty set only after it (dcache_put_now's
 * put-record). Between the two the chunk is in no set a REPORT snapshot
 * reads: a utimens flush for the inode found nothing dirty, its REPORT
 * carried nothing, the SETATTR went out, and the in-flight PUT's chunk
 * was published by the close under the new mtime_gen — mtime = close
 * time (ecopy write→futimens→close, 2 of 5760 files, Sep 30 burst).
 * Every window is counted per inode; efs_dcache_flush_ino waits for the
 * inode's windows to close before it returns. Bounded wait: a PUT that
 * hangs is the RPC deadline's problem, not this one. */
#define PUT_WIN_SLOTS 512
static struct {
    efs_ino_t ino;
    uint32_t n;
} put_win[PUT_WIN_SLOTS];
static uint32_t put_win_overflow;
static uint32_t put_win_total; /* open windows, all inodes: read fast path */
static pthread_mutex_t put_win_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t put_win_cv = PTHREAD_COND_INITIALIZER;

void put_win_open(efs_ino_t ino)
{
    int free_i = -1, i;

    pthread_mutex_lock(&put_win_mu);
    __atomic_fetch_add(&put_win_total, 1, __ATOMIC_RELEASE);
    for (i = 0; i < PUT_WIN_SLOTS; i++) {
        if (put_win[i].n && put_win[i].ino == ino) {
            put_win[i].n++;
            pthread_mutex_unlock(&put_win_mu);
            return;
        }
        if (free_i < 0 && put_win[i].n == 0)
            free_i = i;
    }
    if (free_i >= 0) {
        put_win[free_i].ino = ino;
        put_win[free_i].n = 1;
    } else {
        put_win_overflow++;
    }
    pthread_mutex_unlock(&put_win_mu);
}

void put_win_close(efs_ino_t ino)
{
    int i, found = 0;

    pthread_mutex_lock(&put_win_mu);
    for (i = 0; i < PUT_WIN_SLOTS; i++) {
        if (put_win[i].n && put_win[i].ino == ino) {
            put_win[i].n--;
            found = 1;
            break;
        }
    }
    if (!found && put_win_overflow)
        put_win_overflow--;
    if (put_win_total)
        __atomic_fetch_sub(&put_win_total, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&put_win_cv);
    pthread_mutex_unlock(&put_win_mu);
}

static int put_win_busy_locked(efs_ino_t ino)
{
    int i;

    if (put_win_overflow)
        return 1;
    for (i = 0; i < PUT_WIN_SLOTS; i++)
        if (put_win[i].n && put_win[i].ino == ino)
            return 1;
    return 0;
}

/* 0 = no window was open, 1 = waited and they closed, EFS_ERR_BUSY =
 * still open after max_ms. */
int put_win_wait(efs_ino_t ino, unsigned max_ms)
{
    uint64_t end = put_win_clock_ms() + max_ms;
    uint64_t outer = efs_client_rpc_deadline_ms();
    if (outer && outer < end)
        end = outer;
    int rc = 0;
    pthread_mutex_lock(&put_win_mu);
    while (put_win_busy_locked(ino)) {
        uint64_t now = put_win_clock_ms();
        if (now >= end) {
            rc = EFS_ERR_BUSY;
            break;
        }
        rc = 1;
        uint64_t wait = end - now;
        if (wait > 50) wait = 50;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (long)wait * 1000000;
        dl.tv_sec += dl.tv_nsec / 1000000000;
        dl.tv_nsec %= 1000000000;
        int wrc = pthread_cond_timedwait(&put_win_cv, &put_win_mu, &dl);
        if (wrc && wrc != ETIMEDOUT) {
            rc = EFS_ERR_IO;
            break;
        }
    }
    pthread_mutex_unlock(&put_win_mu);
    return rc;
}

/* Read side of the window. A full-image PUT steals the dcache body
 * (snap-steal-full) and the local chunk map names the object only after
 * the PUT lands, so until then the chunk is in no place a reader looks:
 * dcache_copy misses, the map has no row, and the read is a "hole" of
 * zeros (posix writev_readv_chunk_straddle, Oct 1 07:30Z: W30 PUTs the
 * completed chunk 0 during the writev; the readv raced it). A reader
 * waits for the inode's windows first. Lock-free when none is open. */
int efs_dcache_put_win_wait(efs_ino_t ino)
{
    if (!__atomic_load_n(&put_win_total, __ATOMIC_ACQUIRE))
        return 0;
    return put_win_wait(ino, 8000u);
}
