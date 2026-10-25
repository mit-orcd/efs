#define _GNU_SOURCE
/* Test-only LD_PRELOAD (W159): make every fsync/fdatasync of the process
 * take at least EFS_TEST_FSYNC_DELAY_US microseconds more, and count them.
 * It emulates a slow durable barrier (a consumer NVMe answers a 4 KiB write
 * plus fdatasync in 5-15 ms, an enterprise one in 0.1-0.3 ms) on a fast
 * disk, so one host can show how a fixture's metadata latency scales with
 * the disk's sync latency. Never skips a sync or changes its result. At
 * exit it prints `slow-fsync: pid=P calls=N delay_us=D` to stderr, so the
 * number of durable barriers a workload cost is on record. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static atomic_ulong calls;
static long delay_us = -1;
/* EFS_TEST_FSYNC_LOCK_FILE: hold an exclusive flock on this file while
 * delaying, so every process sharing the file queues on one emulated
 * device (the four members of a one-host fixture share one disk). */
static int lock_fd = -1;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int (*real_fsync)(int), (*real_fdatasync)(int);

static void report(void)
{
    fprintf(stderr, "slow-fsync: pid=%ld calls=%lu delay_us=%ld\n", (long)getpid(),
            atomic_load(&calls), delay_us < 0 ? 0 : delay_us);
}

static void resolve(void)
{
    const char *e = getenv("EFS_TEST_FSYNC_DELAY_US");
    real_fsync = dlsym(RTLD_NEXT, "fsync");
    real_fdatasync = dlsym(RTLD_NEXT, "fdatasync");
    delay_us = e && *e ? strtol(e, NULL, 10) : 0;
    if (delay_us < 0)
        delay_us = 0;
    e = getenv("EFS_TEST_FSYNC_LOCK_FILE");
    if (e && *e)
        lock_fd = open(e, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    atexit(report);
}

static void delay(void)
{
    int saved = errno;
    /* efsd leaves through _exit on a stop, so atexit alone would lose the
     * count; every 100th call prints the running total as well. */
    if (atomic_fetch_add(&calls, 1) % 100 == 99)
        report();
    if (delay_us > 0) {
        struct timespec rem = { delay_us / 1000000, (delay_us % 1000000) * 1000 };
        if (lock_fd >= 0)
            while (flock(lock_fd, LOCK_EX) < 0 && errno == EINTR) {}
        while (nanosleep(&rem, &rem) < 0 && errno == EINTR) {}
        if (lock_fd >= 0)
            (void)flock(lock_fd, LOCK_UN);
    }
    errno = saved;
}

int fsync(int fd)
{
    pthread_once(&once, resolve);
    if (!real_fsync) { errno = ENOSYS; return -1; }
    delay();
    return real_fsync(fd);
}

int fdatasync(int fd)
{
    pthread_once(&once, resolve);
    if (!real_fdatasync) { errno = ENOSYS; return -1; }
    delay();
    return real_fdatasync(fd);
}
