#include "efs/telemetry.h"
#include "efs/log_ts.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define OUTPUT_BYTES 65536
/* Routine daemon lines (K79) share the mailbox with the periodic records, so
 * it holds a burst of them; every drop is still counted. */
#define SLOTS 32
#define LINE_BYTES 2048
/* After stop, pending records get this long on a sink that accepts them; a
 * sink that stays blocked never holds the stop longer. */
#define DRAIN_NS 200000000ull
struct telemetry_item {
    unsigned kind;
    size_t bytes;
    union { uint64_t align; unsigned char bytes[EFS_TELEMETRY_BYTES]; } data;
};
struct efs_telemetry {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t tid;
    int fd, stop, pending, inflight, timestamps, socket_sink;
    uint64_t deadline_ns;
    unsigned head;
    uint64_t overflow, errors;
    efs_telemetry_render render;
    struct telemetry_item queue[SLOTS];
};

/* The binding lock protects lifetime only. Producers try it and never wait;
 * stop unbinds before freeing, so a transport destructor cannot use a freed
 * reporter. No sink I/O is done while this lock is held. */
static pthread_rwlock_t binding_mu = PTHREAD_RWLOCK_INITIALIZER;
static struct efs_telemetry *bound;
static uint64_t binding_overflow;

/* Own file description: O_NONBLOCK on dup(2) would also change the critical
 * synchronous stderr path. /proc/self/fd opens a separate description on Linux.
 * Never fall back to writing the blocking descriptor from the reporter. */
static int stopping(struct efs_telemetry *r)
{
    return __atomic_load_n(&r->stop, __ATOMIC_ACQUIRE);
}

static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Stopping and past the drain deadline: give up on the sink. */
static int expired(struct efs_telemetry *r)
{
    return stopping(r) && mono_ns() >= __atomic_load_n(&r->deadline_ns, __ATOMIC_ACQUIRE);
}

static int send_bytes(struct efs_telemetry *r, const char *p, size_t n)
{
    while (n && !expired(r)) {
        ssize_t w = r->socket_sink
            ? send(r->fd, p, n, MSG_DONTWAIT | MSG_NOSIGNAL)
            : write(r->fd, p, n);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd fd = {r->fd, POLLOUT, 0};
            (void)poll(&fd, 1, 25);
            continue;
        }
        return -1;
    }
    return n ? -1 : 0;
}

static int emit(struct efs_telemetry *r, const char *p, size_t n)
{
    int rc = 0;
    /* Keep whole records ordered against critical fprintf calls. Unlike the
     * pump, this thread may wait for the sink, but shutdown remains interruptible.
     * trylock is essential: a critical logger can already be blocked in stdio. */
    while (ftrylockfile(stderr)) {
        if (expired(r)) return -1;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    while (n && !expired(r)) {
        const char *nl = memchr(p, '\n', n);
        size_t len = nl ? (size_t)(nl-p)+1 : n;
        if (r->timestamps) {
            char prefix[40];
            struct timespec t;
            struct tm tm;
            clock_gettime(CLOCK_REALTIME, &t);
            gmtime_r(&t.tv_sec, &tm);
            int z = snprintf(prefix, sizeof(prefix), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ ",
                             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
                             tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec/1000000);
            if (z < 0 || (size_t)z >= sizeof(prefix) || send_bytes(r, prefix, (size_t)z)) { rc=-1; break; }
        }
        if (send_bytes(r, p, len)) { rc=-1; break; }
        p += len; n -= len;
    }
    funlockfile(stderr);
    return rc || n ? -1 : 0;
}

static void *report(void *arg)
{
    struct efs_telemetry *r = arg;
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    struct telemetry_item item;
    char output[OUTPUT_BYTES];
    for (;;) {
        pthread_mutex_lock(&r->mu);
        while (!r->pending && !stopping(r)) pthread_cond_wait(&r->cv, &r->mu);
        if (!r->pending || expired(r)) { pthread_mutex_unlock(&r->mu); break; }
        struct telemetry_item *slot = &r->queue[r->head];
        item.kind = slot->kind;
        memcpy(item.data.bytes, slot->data.bytes, slot->bytes);
        r->head = (r->head+1) % SLOTS;
        r->pending--;
        r->inflight = 1;
        pthread_mutex_unlock(&r->mu);
        FILE *f = fmemopen(output, sizeof(output), "w");
        if (!f) { __atomic_add_fetch(&r->errors, 1, __ATOMIC_RELAXED); goto done; }
        uint64_t lost = __atomic_load_n(&r->overflow, __ATOMIC_RELAXED) +
                        __atomic_load_n(&binding_overflow, __ATOMIC_RELAXED);
        uint64_t errors = __atomic_load_n(&r->errors, __ATOMIC_RELAXED);
        if (lost || errors)
            fprintf(f, "telemetry: overflow=%llu sink_errors=%llu (cumulative)\n",
                    (unsigned long long)lost, (unsigned long long)errors);
        if (item.kind) fputs((const char *)item.data.bytes, f);
        else r->render(f, item.data.bytes);
        int failed = fflush(f);
        long n = ftell(f);
        failed |= ferror(f);
        fclose(f);
        if (failed || n < 0 || (size_t)n >= sizeof(output))
            __atomic_add_fetch(&r->overflow, 1, __ATOMIC_RELAXED);
        else if (emit(r, output, (size_t)n))
            __atomic_add_fetch(&r->errors, 1, __ATOMIC_RELAXED);
done:
        pthread_mutex_lock(&r->mu);
        r->inflight = 0;
        pthread_mutex_unlock(&r->mu);
    }
    return NULL;
}

struct efs_telemetry *efs_telemetry_start(efs_telemetry_render render)
{
    struct efs_telemetry *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    struct stat st;
    if (fstat(STDERR_FILENO, &st)) { free(r); return NULL; }
    /* Regular files must share the original offset (shell redirection need
     * not use O_APPEND). O_NONBLOCK has no effect on regular-file writes. */
    r->socket_sink = S_ISSOCK(st.st_mode);
    r->fd = (S_ISREG(st.st_mode) || r->socket_sink)
        ? fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3)
        : open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (r->fd < 0) { free(r); return NULL; }
    r->timestamps = efs_log_timestamps_enabled();
    r->render = render;
    if (pthread_mutex_init(&r->mu, NULL)) goto fail_fd;
    if (pthread_cond_init(&r->cv, NULL)) goto fail_mu;
    if (pthread_create(&r->tid, NULL, report, r)) goto fail_cv;
    pthread_rwlock_wrlock(&binding_mu);
    if (!bound) bound = r;
    pthread_rwlock_unlock(&binding_mu);
    return r;
fail_cv: pthread_cond_destroy(&r->cv);
fail_mu: pthread_mutex_destroy(&r->mu);
fail_fd: close(r->fd); free(r); return NULL;
}

/* BLOCK: routine lines from many threads (efs_telemetry_log) wait for the
 * mailbox lock, which no thread holds across I/O or a wait: only a copy in
 * or out. The pump's periodic records and printf lines keep the trylock and
 * never wait. */
static void enqueue(struct efs_telemetry *r, unsigned kind, const void *data, size_t n, int block)
{
    if (!r) return;
    if (n > EFS_TELEMETRY_BYTES || (block ? pthread_mutex_lock(&r->mu) : pthread_mutex_trylock(&r->mu))) {
        __atomic_add_fetch(&r->overflow, 1, __ATOMIC_RELAXED);
        return;
    }
    /* Periodic records replace only an older periodic record, never an event.
     * FIFO order of distinct records and synchronous critical events stays. */
    unsigned pos = (r->head + (unsigned)r->pending) % SLOTS;
    int replace = 0;
    if (!kind) for (int i=0; i<r->pending; i++) {
        unsigned old = (r->head+(unsigned)i) % SLOTS;
        if (!r->queue[old].kind) { pos=old; replace=1; break; }
    }
    if (replace || r->pending == SLOTS)
        __atomic_add_fetch(&r->overflow, 1, __ATOMIC_RELAXED);
    if (!replace && r->pending == SLOTS) {
        pthread_mutex_unlock(&r->mu);
        return;
    }
    r->queue[pos].kind = kind;
    r->queue[pos].bytes = n;
    memcpy(r->queue[pos].data.bytes, data, n);
    if (!replace) r->pending++;
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->mu);
}

void efs_telemetry_submit(struct efs_telemetry *r, const void *data, size_t n)
{
    enqueue(r, 0, data, n, 0);
}

void efs_telemetry_printf(struct efs_telemetry *r, const char *format, ...)
{
    if (!r) return;
    char line[LINE_BYTES];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        __atomic_add_fetch(&r->overflow, 1, __ATOMIC_RELAXED);
        return;
    }
    enqueue(r, 1, line, (size_t)n+1, 0);
}

void efs_telemetry_log(int critical, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    if (critical) {
        vfprintf(stderr, format, ap);
    } else if (pthread_rwlock_rdlock(&binding_mu), !bound) {
        pthread_rwlock_unlock(&binding_mu);
        vfprintf(stderr, format, ap);
    } else {
        char line[LINE_BYTES];
        int n = vsnprintf(line, sizeof(line), format, ap);
        if (n < 0 || (size_t)n >= sizeof(line))
            __atomic_add_fetch(&bound->overflow, 1, __ATOMIC_RELAXED);
        else
            enqueue(bound, 1, line, (size_t)n+1, 1);
        pthread_rwlock_unlock(&binding_mu);
    }
    va_end(ap);
}

/* A fault-hook exit (efs_fault_exit) must not lose the routine lines queued
 * before it: wait until the bound reporter has written them, at most DRAIN_NS
 * (a blocked sink never holds the exit longer). No-op without a reporter. */
void efs_telemetry_flush(void)
{
    uint64_t deadline = mono_ns() + DRAIN_NS;
    pthread_rwlock_rdlock(&binding_mu);
    struct efs_telemetry *r = bound;
    while (r) {
        pthread_mutex_lock(&r->mu);
        int busy = r->pending || r->inflight;
        pthread_mutex_unlock(&r->mu);
        if (!busy || mono_ns() >= deadline) break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    pthread_rwlock_unlock(&binding_mu);
}

void efs_telemetry_stop(struct efs_telemetry *r)
{
    if (!r) return;
    pthread_rwlock_wrlock(&binding_mu);
    if (bound == r) bound = NULL;
    pthread_rwlock_unlock(&binding_mu);
    pthread_mutex_lock(&r->mu);
    __atomic_store_n(&r->deadline_ns, mono_ns() + DRAIN_NS, __ATOMIC_RELEASE);
    __atomic_store_n(&r->stop, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->mu);
    pthread_join(r->tid, NULL);
    close(r->fd);
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mu);
    free(r);
}
