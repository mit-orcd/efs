#include "../common/parse_host_port.h"
#include "efs/cli_help.h"
#include "efs/common.h"
#include "efs/storage_capacity.h"
#include "efs/log_ts.h"
#include "efs/network.h"
#include "efs/version.h"
#include "server_internal.h"
#include "discovery.h"
#include <arpa/inet.h>
#include "storage_roots.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <execinfo.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../common/recorder_stop.h"
#include "../common/recorder_start.h"
#include <sys/resource.h>
#include <time.h>

struct efsd_server *g_server = NULL;


/* Alternate stack so SIGSEGV from stack overflow can still report. */
static char g_efsd_altstack[256 * 1024];

/* Crash diagnostics are best effort: retry interruption, but a short/error
 * stderr write cannot be recovered here. Preserve the interrupted errno. */
static void efsd_diagnostic_write(const void *data, size_t len)
{
    int saved_errno = errno;
    ssize_t written;
    do { written = write(STDERR_FILENO, data, len); }
    while (written < 0 && errno == EINTR);
    errno = saved_errno;
}

static void efsd_fatal_signal(int sig)
{
    const char *name = "signal";
    if (sig == SIGSEGV)
        name = "SIGSEGV";
    else if (sig == SIGBUS)
        name = "SIGBUS";
    else if (sig == SIGABRT)
        name = "SIGABRT";
    else if (sig == SIGILL)
        name = "SIGILL";
    else if (sig == SIGFPE)
        name = "SIGFPE";
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "efsd: fatal %s (%d)\n", name, sig);
    if (n > 0)
        efsd_diagnostic_write(buf, (size_t)n);
    {
        void *frames[64];
        int nf = backtrace(frames, 64);
        backtrace_symbols_fd(frames, nf, STDERR_FILENO);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void efsd_install_crash_handlers(void)
{
#ifdef __SANITIZE_ADDRESS__
    /* Let ASAN own fatal signals so we get a full report. */
    (void)g_efsd_altstack;
    (void)efsd_fatal_signal;
    return;
#endif
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_efsd_altstack;
    ss.ss_size = sizeof(g_efsd_altstack);
    if (sigaltstack(&ss, NULL) != 0)
        fprintf(stderr, "Warning: sigaltstack failed: %s\n", strerror(errno));

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = efsd_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

static void usage(const char *prog)
{
    efs_cli_help(stderr,prog,NULL);
}

static int add_storage_path(struct efsd_server *s, const char *path)
{
    if (!path || !*path)
        return -1;
    if (s->storage_path_count >= EFS_MAX_STORAGE_PATHS) {
        fprintf(stderr, "Too many --storage paths (max %d)\n", EFS_MAX_STORAGE_PATHS);
        return -1;
    }
    strncpy(s->storage_paths[s->storage_path_count], path,
            sizeof(s->storage_paths[0]) - 1);
    s->storage_paths[s->storage_path_count][sizeof(s->storage_paths[0]) - 1] = '\0';
    s->storage_path_count++;
    return 0;
}

static int parse_storage_arg(struct efsd_server *s, const char *arg)
{
    char buf[EFS_MAX_PATH * EFS_MAX_STORAGE_PATHS];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        if (!*tok)
            continue;
        if (add_storage_path(s, tok) != 0)
            return -1;
    }
    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[8192];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static pid_t g_perf_pid = -1;

/* --strace: `strace -f -tt -T -o <path> -p <pid>`. ptrace stops every
 * syscall of every thread, so this is for one node and a wall question
 * (where did a REPORT's finish_ms go), never for a bandwidth number.
 * Needs kernel.yama.ptrace_scope=0 (runtime on the test hosts).
 * EFS_STRACE_EXPR narrows it, e.g. "trace=fsync,fdatasync,pwrite64,futex"
 * (passed as -e). */
static pid_t g_strace_pid = -1;

/* SIGINT/SIGTERM makes perf finalize perf.data and strace detach; wait
 * up to ~5 s, then SIGKILL. */
static void stop_recorder(pid_t *pidp)
{
    efs_stop_recorder(pidp);
}

static void stop_perf_recorder(void)
{
    stop_recorder(&g_strace_pid);
    stop_recorder(&g_perf_pid);
}

static int parse_host_port(const char *str, char *host, size_t host_len, uint16_t *port)
{
    return efs_parse_host_port(str, host, host_len, port);
}

/* Live handler threads (accept path). Used to cap concurrent conns. */
static volatile int g_live_conns;
/* Registered before thread creation, removed before close. Shutdown never
 * operates on a stale descriptor that another thread may have reused. */
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_conn_fds[EFS_SERVER_MAX_CONNS]; /* fd + 1; zero means free */

static int conn_register(int fd)
{
    int found = 0;
    pthread_mutex_lock(&g_conn_lock);
    for (unsigned i = 0; i < EFS_SERVER_MAX_CONNS; i++) {
        if (!g_conn_fds[i]) { g_conn_fds[i] = fd + 1; found = 1; break; }
    }
    pthread_mutex_unlock(&g_conn_lock);
    return found;
}

static void conn_unregister(int fd)
{
    pthread_mutex_lock(&g_conn_lock);
    for (unsigned i = 0; i < EFS_SERVER_MAX_CONNS; i++) {
        if (g_conn_fds[i] == fd + 1) { g_conn_fds[i] = 0; break; }
    }
    pthread_mutex_unlock(&g_conn_lock);
}

static void conn_wake_readers(void)
{
    pthread_mutex_lock(&g_conn_lock);
    for (unsigned i = 0; i < EFS_SERVER_MAX_CONNS; i++)
        if (g_conn_fds[i]) (void)shutdown(g_conn_fds[i] - 1, SHUT_RD);
    pthread_mutex_unlock(&g_conn_lock);
}


/* K1/W145 part 4 and R4: the whole stop is bounded, not only the handler
 * drain. stop.sh stops each daemon through scripts/server_processes.py
 * stop-file, which waits 60 s for the process to exit and then gives up
 * without SIGKILL. One deadline, EFS_SHUTDOWN_TOTAL_MS from the moment
 * SIGTERM/SIGINT is received, covers every phase: handlers get at most four fifths of
 * it (server_handlers_begin_stop closes each connection before its next
 * request; a group read or apply wait is bounded at 400 ms; forwarded
 * mutations by their RPC timeout), and each later stop phase runs on one
 * teardown thread and gets whatever remains of the total.
 * If a handler or a phase's worker is still live at the deadline, efsd
 * refuses to tear shared state down under it (a use-after-free), names the
 * phase and the worker, writes the exit record and exits without teardown,
 * rc EFS_SHUTDOWN_FORCED_RC. Stores are never deleted or modified on that
 * path: durable state needs no teardown, the Raft log is fsynced before any
 * acknowledgement and replays at start.
 * The exit record, <first root>/log/efsd.exit, states the outcome and
 * whether each durability barrier (PUT sync, reclaim journal, usage flush)
 * completed, so the stop script can tell an orderly exit from a forced
 * one. rc 0 is orderly with every barrier done; rc 4 is a completed
 * teardown whose barrier failed.
 * T2: the perf/strace recorders stop in the first teardown phase, inside
 * the same deadline. After the deadline nothing waits on storage or stdio:
 * the exit record and the stderr lines are written by helper threads, the
 * exiting thread waits at most EFS_SHUTDOWN_REPORT_MS for them and then
 * calls _exit(3) whether or not they finished; a record that could not be
 * written in time reads as "unknown" to the stop script. Signal to exit is
 * therefore at most EFS_SHUTDOWN_TOTAL_MS + EFS_SHUTDOWN_REPORT_MS = 55 s
 * plus scheduling, 5 s inside the stop script's 60 s wait. (A thread in an
 * uninterruptible kernel wait still delays the kernel's process teardown;
 * no user-space bound can cover that.) */
#define EFS_SHUTDOWN_TOTAL_MS 50000u
#define EFS_SHUTDOWN_REPORT_MS 5000u
#define EFS_SHUTDOWN_FORCED_RC 3
#define EFS_SHUTDOWN_BARRIER_RC 4
#define EFS_SHUTDOWN_EXIT_RECORD "log/efsd.exit"

static uint64_t shutdown_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* Handlers' share of the total; the rest is the teardown's. */
static unsigned shutdown_drain_share(unsigned total_ms)
{
    return total_ms / 5u * 4u;
}

static unsigned shutdown_total_ms(void)
{
#if EFS_FAULTS
    /* Test-only: shorten the bound so a stuck-worker test runs quickly. */
    const char *v = getenv("EFS_FAULT_SHUTDOWN_TOTAL_MS");
    if (v && *v) {
        unsigned long ms = strtoul(v, NULL, 10);
        if (ms >= 1000 && ms <= EFS_SHUTDOWN_TOTAL_MS) return (unsigned)ms;
    }
#endif
    return EFS_SHUTDOWN_TOTAL_MS;
}

/* Test-only (EFS_FAULTS builds): EFS_FAULT_SHUTDOWN_PARK=<worker> parks
 * that worker forever once the stop begins. Production has no such hook. */
int server_shutdown_fault_park(const char *worker)
{
#if EFS_FAULTS
    const char *v = getenv("EFS_FAULT_SHUTDOWN_PARK");
    if (!v || strcmp(v, worker)) return 0;
    fprintf(stderr, "shutdown: injected park of %s\n", worker);
    for (;;) sleep(60);
#else
    (void)worker;
    return 0;
#endif
}

/* Stop-time messages never go through stdio on the stopping thread: a
 * full stderr pipe, or another thread blocked while holding stderr's
 * lock, would hold the stop past its deadline. Each line goes out on a
 * detached helper thread; one that blocks is abandoned at exit. */
static void *shutdown_say_thread(void *arg)
{
    fputs(arg, stderr);
    fflush(stderr);
    free(arg);
    return NULL;
}

__attribute__((format(printf, 1, 2)))
static void shutdown_say(const char *fmt, ...)
{
    char *line = malloc(512);
    if (!line) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, 512, fmt, ap);
    va_end(ap);
    pthread_t tid;
    if (efsd_pthread_create(&tid, shutdown_say_thread, line) != 0) {
        free(line);
        return;
    }
    pthread_detach(tid);
}

/* Waits for every handler until DEADLINE (monotonic ms); -1 if any is
 * still live then. */
static int shutdown_drain(uint64_t t0, uint64_t deadline)
{
    uint64_t next_note = t0 + 5000;

    for (;;) {
        int live = __sync_add_and_fetch(&g_live_conns, 0);
        if (live <= 0) return 0;
        uint64_t now = shutdown_now_ms();
        if (now >= deadline) return -1;
        if (now >= next_note) {
            shutdown_say("shutdown: waiting for %d live handlers (%llu of %llu ms)\n", live,
                         (unsigned long long)(now - t0), (unsigned long long)(deadline - t0));
            next_note += 5000;
        }
        usleep(50 * 1000);
    }
}

/* Durability barriers the exit record reports. */
enum { SD_PUT_SYNC, SD_RECLAIM, SD_USAGE, SD_NBARRIERS };
enum { SD_NOT_REACHED, SD_DONE, SD_FAILED, SD_STUCK };
static const char *const sd_barrier_names[SD_NBARRIERS] = {
    "put_sync", "reclaim_journal", "usage_flush"
};
static const char *const sd_state_names[] = { "not_reached", "done", "failed", "stuck" };

static int sd_recorders(struct efsd_server *s) { (void)s; stop_perf_recorder(); return 0; }
static int sd_packer(struct efsd_server *s) { (void)s; server_packer_stop(); return 0; }
static int sd_writers(struct efsd_server *s) { server_writer_pool_stop(s); return 0; }
static int sd_put_sync(struct efsd_server *s) { return server_put_sync_stop(s); }
static int sd_reclaim(struct efsd_server *s) { server_reclaim_shutdown(s); return 0; }
static int sd_usage(struct efsd_server *s)
{
    server_usage_flush_dirty(s);
    /* A failed save marks the usage dirty again. */
    return __atomic_load_n(&s->usage_dirty, __ATOMIC_ACQUIRE) ? -1 : 0;
}
/* Nothing to replay on the way down: the Raft log is the durability
 * boundary and recovery is WAL replay at next start. */
static int sd_raft(struct efsd_server *s) { (void)s; server_raft_host_stop(); return 0; }
static int sd_peers(struct efsd_server *s) { (void)s; server_peer_pool_shutdown(); return 0; }
static int sd_roots(struct efsd_server *s) { server_storage_roots_close(s); return 0; }

static const struct shutdown_phase {
    const char *name;
    const char *worker;   /* what a stuck phase is waiting for */
    int (*run)(struct efsd_server *s);
    int barrier;          /* SD_* index, or -1 */
} g_sd_phases[] = {
    { "recorders", "perf/strace recorder processes", sd_recorders, -1 },
    { "packer", "packer thread", sd_packer, -1 },
    { "writer-pool", "writer threads", sd_writers, -1 },
    { "put-sync", "PUT sync committer and scanner threads", sd_put_sync, SD_PUT_SYNC },
    { "reclaim", "reclaim journal, deleter and recovery threads", sd_reclaim, SD_RECLAIM },
    { "usage-flush", "usage record write", sd_usage, SD_USAGE },
    { "raft-host", "Raft host threads", sd_raft, -1 },
    { "peer-pool", "peer pool connections", sd_peers, -1 },
    { "storage-roots", "storage root close", sd_roots, -1 },
};
#define SD_NPHASES (sizeof(g_sd_phases) / sizeof(g_sd_phases[0]))

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct efsd_server *s;
    unsigned phase;          /* running phase; SD_NPHASES when done */
    int barrier[SD_NBARRIERS];
} g_sd = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void *shutdown_teardown_thread(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < SD_NPHASES; i++) {
        pthread_mutex_lock(&g_sd.mu);
        g_sd.phase = i;
        pthread_mutex_unlock(&g_sd.mu);
        int rc = g_sd_phases[i].run(g_sd.s);
        pthread_mutex_lock(&g_sd.mu);
        if (g_sd_phases[i].barrier >= 0)
            g_sd.barrier[g_sd_phases[i].barrier] = rc == 0 ? SD_DONE : SD_FAILED;
        pthread_mutex_unlock(&g_sd.mu);
    }
    pthread_mutex_lock(&g_sd.mu);
    g_sd.phase = SD_NPHASES;
    pthread_cond_broadcast(&g_sd.cv);
    pthread_mutex_unlock(&g_sd.mu);
    return NULL;
}

/* The exit report: one stderr line and the same fields in the exit
 * record. Both are written on detached helper threads; the caller waits
 * at most EFS_SHUTDOWN_REPORT_MS and goes on whether or not they finished,
 * so a stalled storage root or a blocked stderr cannot hold the exit. A
 * record that is not written in time, or not at all, leaves the outcome
 * unknown to the stop script, which then never reports an orderly stop.
 * One report per process. */
static struct {
    char path[8192], tmp[8300];
    char record[1024];
    size_t record_len;
    char line[2048];
    int done;                /* helpers finished */
} g_report;

/* Test-only (EFS_FAULTS builds): EFS_FAULT_SHUTDOWN_RECORD_PARK=write or
 * =fsync parks the exit-record helper forever before that step, as a
 * stalled storage root would. */
static void shutdown_fault_record_park(const char *step)
{
#if EFS_FAULTS
    const char *v = getenv("EFS_FAULT_SHUTDOWN_RECORD_PARK");
    if (v && !strcmp(v, step))
        for (;;) sleep(60);
#else
    (void)step;
#endif
}

static void *shutdown_record_thread(void *arg)
{
    (void)arg;
    int fd = open(g_report.tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int ok = fd >= 0;
    if (ok) shutdown_fault_record_park("write");
    for (size_t off = 0; ok && off < g_report.record_len;) {
        ssize_t w = write(fd, g_report.record + off, g_report.record_len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = 0; else off += (size_t)w;
    }
    if (ok) shutdown_fault_record_park("fsync");
    if (ok && fsync(fd) != 0) ok = 0;
    if (fd >= 0 && close(fd) != 0) ok = 0;
    if (ok && rename(g_report.tmp, g_report.path) != 0) ok = 0;
    if (!ok) {
        int e = errno;
        (void)unlink(g_report.tmp);
        fprintf(stderr, "shutdown: could not write exit record %s: %s\n", g_report.path,
                strerror(e));
    }
    __atomic_add_fetch(&g_report.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *shutdown_line_thread(void *arg)
{
    (void)arg;
    fputs(g_report.line, stderr);
    fflush(stderr);
    fflush(stdout);
    __atomic_add_fetch(&g_report.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* PREFACE, if set, is stderr text written before the outcome line. Returns
 * 0 when both helpers finished within the bound. */
static int shutdown_report(const struct efsd_server *s, const char *preface,
                           const char *outcome, int rc, const char *phase,
                           const char *worker, uint64_t elapsed_ms, unsigned total_ms,
                           const int barrier[SD_NBARRIERS])
{
    char *buf = g_report.record;
    const size_t cap = sizeof(g_report.record);
    int n = snprintf(buf, cap,
                     "efsd-exit 1\npid=%ld\noutcome=%s\nrc=%d\nphase=%s\nworker=%s\n"
                     "elapsed_ms=%llu\ndeadline_ms=%u\n",
                     (long)getpid(), outcome, rc, phase, worker,
                     (unsigned long long)elapsed_ms, total_ms);
    for (int b = 0; b < SD_NBARRIERS && n > 0 && (size_t)n < cap; b++)
        n += snprintf(buf + n, cap - (size_t)n, "%s=%s\n",
                      sd_barrier_names[b], sd_state_names[barrier[b]]);
    if (n > 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - (size_t)n, "stores=%s\n",
                      rc == EFS_SHUTDOWN_FORCED_RC ? "preserved" : "closed");
    if (n <= 0 || (size_t)n >= cap) {
        shutdown_say("%sshutdown: exit record overflow\n", preface ? preface : "");
        return -1;
    }
    g_report.record_len = (size_t)n;
    snprintf(g_report.line, sizeof(g_report.line),
             "%sshutdown: outcome=%s rc=%d phase=%s worker=\"%s\" elapsed_ms=%llu "
             "deadline_ms=%u put_sync=%s reclaim_journal=%s usage_flush=%s\n",
             preface ? preface : "", outcome, rc, phase, worker,
             (unsigned long long)elapsed_ms, total_ms,
             sd_state_names[barrier[SD_PUT_SYNC]], sd_state_names[barrier[SD_RECLAIM]],
             sd_state_names[barrier[SD_USAGE]]);
    snprintf(g_report.path, sizeof(g_report.path), "%s/" EFS_SHUTDOWN_EXIT_RECORD,
             s->storage_path);
    snprintf(g_report.tmp, sizeof(g_report.tmp), "%s.tmp.%ld", g_report.path, (long)getpid());

    int started = 0;
    pthread_t tid;
    if (efsd_pthread_create(&tid, shutdown_line_thread, NULL) == 0) {
        pthread_detach(tid);
        started++;
    }
    if (efsd_pthread_create(&tid, shutdown_record_thread, NULL) == 0) {
        pthread_detach(tid);
        started++;
    }
    const uint64_t end = shutdown_now_ms() + EFS_SHUTDOWN_REPORT_MS;
    while (__atomic_load_n(&g_report.done, __ATOMIC_ACQUIRE) < started &&
           shutdown_now_ms() < end)
        usleep(10 * 1000);
    return started == 2 && __atomic_load_n(&g_report.done, __ATOMIC_ACQUIRE) == 2 ? 0 : -1;
}

/* Never returns. After the deadline nothing here waits on storage, stdio
 * locks or fflush beyond the report bound. */
static void shutdown_forced(const struct efsd_server *s, const char *phase,
                            const char *worker, uint64_t elapsed_ms, unsigned total_ms,
                            const int barrier[SD_NBARRIERS])
{
    char preface[512];
    snprintf(preface, sizeof(preface),
             "shutdown: deadline %u ms passed in phase %s with %s still live; "
             "refusing to tear down shared state under it; exiting without teardown, "
             "stores preserved (rc=%d)\n", total_ms, phase, worker, EFS_SHUTDOWN_FORCED_RC);
    (void)shutdown_report(s, preface, "forced", EFS_SHUTDOWN_FORCED_RC, phase, worker,
                          elapsed_ms, total_ms, barrier);
    _exit(EFS_SHUTDOWN_FORCED_RC);
}

/* Runs the stop; T0 is when the stop signal arrived (monotonic ms).
 * Returns the exit status of an orderly or barrier-failed stop; a forced
 * stop never returns. */
static int shutdown_bounded(struct efsd_server *s, uint64_t t0)
{
    const unsigned total_ms = shutdown_total_ms();
    const uint64_t end = t0 + total_ms;
    int none[SD_NBARRIERS] = { SD_NOT_REACHED, SD_NOT_REACHED, SD_NOT_REACHED };

    /* Busy handlers close before their next request; idle pooled readers
     * have no receive timeout, so wake them. Shared state stays until every
     * handler and its TLS cleanup has completed. */
    server_handlers_begin_stop();
    conn_wake_readers();
    if (shutdown_drain(t0, t0 + shutdown_drain_share(total_ms)) != 0) {
        char worker[64];
        snprintf(worker, sizeof(worker), "%d connection handlers",
                 __sync_add_and_fetch(&g_live_conns, 0));
        shutdown_forced(s, "handler-drain", worker, shutdown_now_ms() - t0, total_ms, none);
    }

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g_sd.cv, &ca);
    pthread_condattr_destroy(&ca);
    g_sd.s = s;
    g_sd.phase = 0;
    for (int b = 0; b < SD_NBARRIERS; b++) g_sd.barrier[b] = SD_NOT_REACHED;
    pthread_t tid;
    if (efsd_pthread_create(&tid, shutdown_teardown_thread, NULL) != 0)
        shutdown_forced(s, "teardown-start", "teardown thread (create failed)",
                        shutdown_now_ms() - t0, total_ms, none);

    struct timespec due = { .tv_sec = (time_t)(end / 1000u),
                            .tv_nsec = (long)(end % 1000u) * 1000000L };
    pthread_mutex_lock(&g_sd.mu);
    while (g_sd.phase < SD_NPHASES) {
        int rc = pthread_cond_timedwait(&g_sd.cv, &g_sd.mu, &due);
        if (rc == ETIMEDOUT && g_sd.phase < SD_NPHASES) {
            const struct shutdown_phase *p = &g_sd_phases[g_sd.phase];
            int barrier[SD_NBARRIERS];
            memcpy(barrier, g_sd.barrier, sizeof(barrier));
            if (p->barrier >= 0) barrier[p->barrier] = SD_STUCK;
            /* The teardown thread may be inside shared state: never unlock
             * into it, never join it. */
            shutdown_forced(s, p->name, p->worker, shutdown_now_ms() - t0, total_ms, barrier);
        }
    }
    int barrier[SD_NBARRIERS];
    memcpy(barrier, g_sd.barrier, sizeof(barrier));
    pthread_mutex_unlock(&g_sd.mu);
    pthread_join(tid, NULL);

    int rc = 0;
    for (int b = 0; b < SD_NBARRIERS; b++)
        if (barrier[b] != SD_DONE) rc = EFS_SHUTDOWN_BARRIER_RC;
    /* A report still blocked would hold exit()'s stdio flush: leave
     * without it, like the forced path. */
    if (shutdown_report(s, NULL, rc ? "barrier-failed" : "orderly", rc, "done", "none",
                        shutdown_now_ms() - t0, total_ms, barrier) != 0)
        _exit(rc);
    return rc;
}

static void *conn_thread(void *arg)
{
    int fd = (intptr_t)arg;
    /* No SO_RCVTIMEO: a 60s idle timeout FINed pooled client fds and left
     * the FUSE pool in CLOSE-WAIT, so later PUTs never reached the cluster.
     * Wait forever for the next request; keepalive reaps a dead peer. */
    efs_set_recv_timeout(fd, 0);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_tcp_keepalive(fd);
    struct efs_conn *conn = efs_conn_wrap_tcp(fd, 1);
    if (!conn) {
        conn_unregister(fd);
        close(fd);
        __sync_fetch_and_sub(&g_live_conns, 1);
        return NULL;
    }
    server_handle_conn(conn);
    server_shutdown_fault_park("conn");
    conn_unregister(fd);
    efs_conn_destroy(conn); /* QP (if upgraded) + fd */
    server_handler_tls_cleanup();
    __sync_fetch_and_sub(&g_live_conns, 1);
    return NULL;
}

/* When the first SIGTERM/SIGINT arrived (monotonic ms); 0 before. The
 * shutdown deadline counts from here. */
static uint64_t g_stop_signal_ms;

static void sigint_handler(int sig)
{
    (void)sig;
    uint64_t unset = 0;
    /* clock_gettime is async-signal-safe. */
    __atomic_compare_exchange_n(&g_stop_signal_ms, &unset, shutdown_now_ms(), 0,
                                __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    /* Async-signal-safe only: flip the flag. Do not close(listen_fd) here —
     * racing accept()/other threads on a closed fd has segfaulted on Engaging.
     * Without SA_RESTART, accept() returns EINTR and the loop exits. */
    if (g_server)
        g_server->running = 0;
}

/* K33 discovery answer (discovery.h): this node's service endpoint and
 * what it knows of its cluster. Silent while the host is not running. */
static int discovery_info(struct efs_disc_answer *a)
{
    int formed = 0;
    if (!g_server || server_member_view(&a->cluster, &a->count, &formed) != 0)
        return -1;
    a->formed = (uint8_t)formed;
    snprintf(a->addr, sizeof(a->addr), "%s", g_server->addr);
    a->port = g_server->port;
    snprintf(a->build_id, sizeof(a->build_id), "%s", EFS_BUILD_ID);
    return 0;
}

/* Sleep up to ms, returning early when a stop signal arrived. */
static void join_pause(struct efsd_server *s, int ms)
{
    for (int t = 0; t < ms && s->running; t += 100)
        poll(NULL, 0, 100);
}

/* K33: find a cluster on the local subnet. The answers of the one cluster
 * found (formed members first), 0 when nobody answered (said, and the
 * caller waits), or -1 when the operator must name a seed (several
 * clusters, no interface, discovery off). */
static int join_discover(struct efsd_server *s, struct efs_disc_answer *out, int max,
                         uint64_t *noted_ms)
{
    struct efs_disc_target t[EFS_DISC_MAX_TARGETS];
    struct efs_disc_answer a[64];
    char names[1024];
    uint64_t clusters[64];
    int nc = 0, n, k = 0;

    if (!s->discovery_port) {
        fprintf(stderr, "join: no seed given and discovery is off (--discovery-port 0); name "
                "a member: efsd --join ADDR:PORT STORAGE\n");
        return -1;
    }
    int nt = efs_disc_targets(s->discover_on[0] ? s->discover_on : NULL, t,
                              EFS_DISC_MAX_TARGETS, names, sizeof(names));
    if (!nt) {
        fprintf(stderr, "join: no IPv4 interface that is up%s%s to discover a cluster on; name "
                "a member: efsd --join ADDR:PORT STORAGE\n",
                s->discover_on[0] ? " matches --discover-on " : "", s->discover_on);
        return -1;
    }
    n = efs_disc_probe(t, nt, s->discovery_port, 1500, a, 64);
    if (n < 0) {
        fprintf(stderr, "join: cannot send a discovery probe on %s: %s\n", names, strerror(errno));
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        if (strncmp(a[i].build_id, EFS_BUILD_ID, sizeof(a[i].build_id))) {
            fprintf(stderr, "discovery: %s:%u (cluster %016llx) runs build '%s', this node "
                    "'%s'; ignored\n", a[i].addr, a[i].port, (unsigned long long)a[i].cluster,
                    a[i].build_id, EFS_BUILD_ID);
            continue;
        }
        int j;
        for (j = 0; j < nc && clusters[j] != a[i].cluster; j++)
            ;
        if (j == nc && nc < 64)
            clusters[nc++] = a[i].cluster;
        a[k++] = a[i];
    }
    if (nc > 1) {
        fprintf(stderr, "discovery: %d clusters answer on %s (UDP port %u):\n", nc, names,
                s->discovery_port);
        for (int j = 0; j < nc; j++)
            for (int i = 0; i < k; i++)
                if (a[i].cluster == clusters[j])
                    fprintf(stderr, "  cluster %016llx: %s:%u (%s, %u members)\n",
                            (unsigned long long)a[i].cluster, a[i].addr, a[i].port,
                            a[i].formed ? "formed" : "forming", a[i].count);
        fprintf(stderr, "join: several clusters answer; start again naming a member of the one "
                "to join: efsd --join ADDR:PORT STORAGE\n");
        return -1;
    }
    if (!nc) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
        if (!*noted_ms || now - *noted_ms >= 30000) {
            *noted_ms = now;
            fprintf(stderr, "discovery: no EFS cluster answered on %s (UDP port %u); waiting "
                    "and asking again every 5 s. Start the first node with efsd ADDR:PORT "
                    "STORAGE, or name a member on another subnet: efsd --join ADDR:PORT "
                    "STORAGE\n", names, s->discovery_port);
        }
        return 0;
    }
    int m = 0;
    for (int pass = 1; pass >= 0; pass--)
        for (int i = 0; i < k && m < max; i++)
            if (a[i].formed == pass)
                out[m++] = a[i];
    fprintf(stderr, "discovery: found cluster %016llx (%s, %u members) at %d endpoint%s, "
            "first %s:%u\n", (unsigned long long)out[0].cluster,
            out[0].formed ? "formed" : "forming", out[0].count, m, m == 1 ? "" : "s",
            out[0].addr, out[0].port);
    return m;
}

/* K33 (M4', M5): a joining store has no node id. Find a peer (the seed,
 * or one found by discovery), choose this node's endpoint (--listen, or
 * the address of the interface that reaches the peer with the peer's
 * service port) and bind it, then say HELLO every second until the
 * cluster gives this store a node id: a promise for the seed's formation
 * proposal, or the committed revision of its addition (both persisted).
 * 0 (admitted; s->rejoin_addr names the peer), 1 (refused or nothing to
 * join: exit 1) or 2 (stopped by a signal). */
static int join_as_new(struct efsd_server *s, const char *seed_host, uint16_t seed_port,
                       int have_endpoint)
{
    struct efs_disc_answer found[32];
    struct efs_join_note note;
    struct { char addr[64]; uint16_t port; } alt[EFS_MAX_NODES];
    int nfound = 0, next = 0, nalt = 0, cur = -1;
    uint64_t noted_ms = 0;
    time_t last_note = 0;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    s->running = 1;
    while (s->running) {
        char host[64];
        uint16_t port;
        if (cur >= 0) {
            /* Another member an earlier answer named (the contact went away). */
            snprintf(host, sizeof(host), "%s", alt[cur].addr);
            port = alt[cur].port;
        } else if (seed_host) {
            snprintf(host, sizeof(host), "%s", seed_host);
            port = seed_port;
        } else {
            if (next >= nfound) {
                nfound = join_discover(s, found, 32, &noted_ms);
                next = 0;
                if (nfound < 0)
                    return 1;
                if (!nfound) {
                    join_pause(s, 5000);
                    continue;
                }
            }
            snprintf(host, sizeof(host), "%s", found[next].addr);
            port = found[next].port;
            next++;
        }
        if (s->listen_fd < 0) {
            if (!have_endpoint) {
                if (efs_disc_local_addr(host, port, s->addr, sizeof(s->addr)) != 0) {
                    fprintf(stderr, "join: no route to %s:%u; retrying\n", host, port);
                    join_pause(s, 1000);
                    continue;
                }
                s->port = port;
            }
            s->listen_fd = efs_listen_tcp(s->addr, s->port, EFS_LISTEN_BACKLOG);
            if (s->listen_fd < 0) {
                fprintf(stderr, "Failed to listen on %s:%u: %s%s\n", s->addr, s->port,
                        strerror(errno), have_endpoint ? "" :
                        " (this node takes the address that reaches the cluster and its "
                        "service port; give another with --listen ADDR:PORT)");
                return 1;
            }
            if (have_endpoint)
                fprintf(stderr, "join: this node's endpoint is %s:%u (--listen)\n", s->addr,
                        s->port);
            else
                fprintf(stderr, "join: this node's endpoint is %s:%u (the interface that "
                        "reaches %s, the cluster's service port)\n", s->addr, s->port, host);
        }
        int rc = server_join_as_new(s, host, port, &note);
        if (note.npeers) {
            nalt = (int)note.npeers;
            for (int i = 0; i < nalt; i++) {
                snprintf(alt[i].addr, sizeof(alt[i].addr), "%s", note.peer[i].addr);
                alt[i].port = note.peer[i].port;
            }
            if (cur >= nalt)
                cur = -1;
        }
        if (rc == EFS_JOIN_ADMITTED) {
            snprintf(s->rejoin_addr, sizeof(s->rejoin_addr), "%s", host);
            s->rejoin_port = port;
            return 0;
        }
        if (rc == EFS_JOIN_REFUSED_FORMED)
            return 1;
        time_t now = time(NULL);
        if (now - last_note >= 10) {
            last_note = now;
            if (rc == EFS_JOIN_PENDING && note.formed)
                fprintf(stderr, "join: pending at %s:%u: cluster %016llx has %u members; this "
                        "store (%016llx) at %s:%u is being added; retrying every second\n",
                        host, port, (unsigned long long)note.cluster, note.count,
                        (unsigned long long)s->mstore.token, s->addr, s->port);
            else if (rc == EFS_JOIN_PENDING)
                fprintf(stderr, "join: waiting at %s:%u: cluster %016llx is forming (%u "
                        "joined; formation needs %u); retrying every second\n", host, port,
                        (unsigned long long)note.cluster, note.count, EFS_FORMATION_MIN);
            else
                fprintf(stderr, "join: %s:%u does not answer; retrying\n", host, port);
        }
        if (rc != EFS_JOIN_PENDING && nalt) {
            /* Unreachable: ask the next member it named, then the seed (or
             * the endpoints discovery found) again. */
            cur = cur + 1 < nalt ? cur + 1 : -1;
            join_pause(s, 200);
            continue;
        }
        if (rc != EFS_JOIN_PENDING && !seed_host)
            continue; /* the next endpoint found, or discover again */
        join_pause(s, 1000);
    }
    return 2;
}

int main(int argc, char **argv)
{
    efs_version_check_argv("efsd", argc, argv);
    if (efs_help_requested(argc,argv)) { efs_cli_help(stdout,argv[0],NULL);return 0; }

    /* Install before any work — imagenet load has produced silent SIGSEGVs. */
    efsd_install_crash_handlers();
    efs_log_timestamps_install();
#if !EFS_FAULTS
    efs_faults_env_ignored("efsd");
#endif

    /* The 1024-fd soft cap put accept() into EMFILE backoff under conn
     * storms (~500 live conns + data files), which wedged new mounts and
     * resync fetches. Raise to the hard cap (524288) at startup. */
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
                fprintf(stderr, "setrlimit(RLIMIT_NOFILE) failed: %s\n",
                        strerror(errno));
        }
    }

    struct efsd_server server;
    memset(&server, 0, sizeof(server));
    server.persist_nodes = 1;
    server.direct_io = 0; /* default: page cache; opt in with --direct-io */
    server.nwriters = EFS_DEFAULT_WRITERS; /* eight durable I/O workers; --writers overrides */
    server.listen_fd = -1;
    g_server = &server;
    pthread_mutex_init(&server.lock, NULL);
    pthread_cond_init(&server.export_idle_cv, NULL);
    server_peer_pool_init();

    const char *pos[2], *listen_ep = NULL;
    int join = 0, npos = 0;
    char seed_host[64] = "";
    uint16_t seed_port = 0;
    server.discovery_port = EFS_DISCOVERY_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--join") == 0) {
            join = 1;
        } else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_ep = argv[++i];
        } else if (strcmp(argv[i], "--discovery-port") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || v < 0 || v > 65535) {
                fprintf(stderr, "Invalid --discovery-port value (0..65535; 0 turns discovery off)\n");
                usage(argv[0]);
                return 1;
            }
            server.discovery_port = (uint16_t)v;
        } else if (strcmp(argv[i], "--discover-on") == 0 && i + 1 < argc) {
            snprintf(server.discover_on, sizeof(server.discover_on), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--meta-storage") == 0 && i + 1 < argc) {
            strncpy(server.meta_storage, argv[++i],
                    sizeof(server.meta_storage) - 1);
            server.meta_storage[sizeof(server.meta_storage) - 1] = '\0';
        } else if (strcmp(argv[i], "--quota") == 0 && i + 1 < argc) {
            server.quota = efs_parse_quota(argv[++i]);
            if (server.quota == 0) {
                fprintf(stderr, "Invalid --quota value\n");
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--direct-io") == 0) {
            server.direct_io = 1;
        } else if (strcmp(argv[i], "--no-direct-io") == 0) {
            server.direct_io = 0;
        } else if (strcmp(argv[i], "--writers") == 0 && i + 1 < argc) {
            int n = atoi(argv[++i]);
            if (n < 0 || n > EFS_MAX_WRITERS) {
                fprintf(stderr, "Invalid --writers value (0..%d total)\n",
                        EFS_MAX_WRITERS);
                usage(argv[0]);
                return 1;
            }
            server.nwriters = n;
        } else if (strcmp(argv[i], "--no-persist") == 0) {
            server.persist_nodes = 0;
        } else if (strcmp(argv[i], "--perf") == 0) {
            server.perf = 1;
        } else if (strcmp(argv[i], "--strace") == 0) {
            server.strace = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--node-id") || !strcmp(argv[i], "--addr") ||
                   !strcmp(argv[i], "--port") || !strcmp(argv[i], "--storage")) {
            fprintf(stderr, "%s was removed (K33): node ids come from the cluster; start a "
                    "node with efsd ADDR:PORT STORAGE (new cluster or restart) or efsd --join "
                    "[SEED] STORAGE (join)\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (npos < 2) {
            pos[npos++] = argv[i];
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* efsd ADDR:PORT STORAGE      create a cluster (fresh STORAGE) or restart
     * efsd --join [SEED] STORAGE  join (SEED, or discovery) or restart */
    const char *storage = NULL, *endpoint = NULL;
    if (join && npos == 2) {
        storage = pos[1];
        if (parse_host_port(pos[0], seed_host, sizeof(seed_host), &seed_port) != 0) {
            fprintf(stderr, "Invalid seed address: %s (want ADDR:PORT)\n", pos[0]);
            return 1;
        }
    } else if (join && npos == 1) {
        storage = pos[0];
    } else if (!join && npos == 2) {
        endpoint = pos[0];
        storage = pos[1];
    }
    if (!storage) {
        fprintf(stderr, join ? "Missing STORAGE: efsd --join [SEED] STORAGE\n"
                             : "Missing arguments: efsd ADDR:PORT STORAGE, or efsd --join "
                               "[SEED] STORAGE\n");
        usage(argv[0]);
        return 1;
    }
    if (listen_ep && !join) {
        fprintf(stderr, "--listen is for --join; a new cluster's node is given as ADDR:PORT\n");
        return 1;
    }
    if (listen_ep)
        endpoint = listen_ep;
    if (endpoint) {
        struct in_addr ia;
        if (parse_host_port(endpoint, server.addr, sizeof(server.addr), &server.port) != 0 ||
            inet_pton(AF_INET, server.addr, &ia) != 1 || ia.s_addr == htonl(INADDR_ANY)) {
            fprintf(stderr, "Invalid endpoint %s: want an IPv4 ADDR:PORT this node listens on\n",
                    endpoint);
            return 1;
        }
    }
    if (parse_storage_arg(&server, storage) != 0 || server.storage_path_count == 0) {
        fprintf(stderr, "Invalid STORAGE: %s\n", storage);
        usage(argv[0]);
        return 1;
    }
    /* W153: the member count is the membership committed at formation
     * (efs-mgmt raft-mkfs); a set variable is a stale deployment input. */
    if (getenv("EFS_MD_RAFT_N")) {
        fprintf(stderr, "EFS_MD_RAFT_N was removed (W153): the member count and the metadata "
                "voters come from the membership committed at formation (efs-mgmt raft-mkfs). "
                "Unset it and start again.\n");
        return 1;
    }
    strncpy(server.storage_path, server.storage_paths[0], sizeof(server.storage_path) - 1);
    server.storage_path[sizeof(server.storage_path) - 1] = '\0';

    /* Line-buffer early so join/listen banners show up in harness logs promptly. */
    setlinebuf(stdout);
    setlinebuf(stderr);

    char perf_path[8192];
    perf_path[0] = '\0';
    if (server.perf) {
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(perf_path, sizeof(perf_path), "%s", pp);
        else
            snprintf(perf_path, sizeof(perf_path), "%s/log/perf.data", server.storage_path);
    }
    char strace_path[8192];
    strace_path[0] = '\0';
    if (server.strace) {
        const char *sp = getenv("EFS_STRACE_PATH");
        if (sp && *sp)
            snprintf(strace_path, sizeof(strace_path), "%s", sp);
        else
            snprintf(strace_path, sizeof(strace_path), "%s/log/strace.txt", server.storage_path);
    }

    for (uint32_t pi = 0; pi < server.storage_path_count; pi++) {
        if (mkdir_p(server.storage_paths[pi]) != 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir storage %s: %s\n", server.storage_paths[pi],
                    strerror(errno));
            return 1;
        }
        char subdir[8192];
        snprintf(subdir, sizeof(subdir), "%s/data", server.storage_paths[pi]);
        mkdir_p(subdir);
        snprintf(subdir, sizeof(subdir), "%s/meta", server.storage_paths[pi]);
        mkdir_p(subdir);
        snprintf(subdir, sizeof(subdir), "%s/log", server.storage_paths[pi]);
        mkdir_p(subdir);
    }
    {
        /* A previous incarnation's exit record must not describe this one. */
        char exit_record[8192];
        snprintf(exit_record, sizeof(exit_record), "%s/" EFS_SHUTDOWN_EXIT_RECORD,
                 server.storage_path);
        (void)unlink(exit_record);
    }
    if (server_storage_roots_init(&server) != EFS_OK) return 1;
    if (server_put_sync_init(&server) != EFS_OK) return 1;
    /* Any refusal below exits before serving; it must not leave the roots
     * marked as an unclean boot (that fences COMMIT until W71 recovery). */
    atexit(server_put_sync_abandon);
    fprintf(stderr, "direct_io=%s\n", server.direct_io ? "on" : "off");
    if (server.meta_storage[0] &&
        mkdir_p(server.meta_storage) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir meta-storage %s: %s\n", server.meta_storage,
                strerror(errno));
        return 1;
    }

    {
        /* Roots carrying these retired markers may hold a target write in
         * flight that this build cannot finish or collect: refuse them. */
        const char *markers[]={"repair-active","repair-store","repair-job"};
        for(unsigned i=0;i<sizeof(markers)/sizeof(markers[0]);i++) {
            char marker_path[EFS_MAX_PATH+80];struct stat marker_stat;
            int nn=snprintf(marker_path,sizeof(marker_path),"%s/meta/%s",server.storage_paths[0],markers[i]);
            if(nn<0 || (size_t)nn>=sizeof(marker_path))return 1;
            if(!lstat(marker_path,&marker_stat) || errno!=ENOENT) {
                fprintf(stderr,"storage: %s holds unsupported state %s; refusing\n",
                        server.storage_paths[0],markers[i]);return 1;
            }
        }
    }

    /* Own the member store before anything else touches it, then advance
     * the durable boot counter (the incarnation, the Raft boot id) before
     * anything is served. A refusal leaves the store as it was. K33: a
     * fresh store started with ADDR:PORT becomes a seed (node 1 of a new
     * cluster); one started with --join a joining store (no node id). */
    {
        char mdraft[EFS_MAX_PATH + 16], err[1024];
        const char *meta_root = server.meta_storage[0] ? server.meta_storage : server.storage_path;
        snprintf(mdraft, sizeof(mdraft), "%s/mdraft", meta_root);
        if (member_store_open(&server.mstore, mdraft, !join, server.addr, server.port, err,
                              sizeof(err)) != EFS_OK) {
            fprintf(stderr, "member store refused: %s\n", err);
            return 1;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t fresh = ((uint64_t)getpid() << 32) ^ ((uint64_t)ts.tv_sec << 16) ^
                         (uint64_t)ts.tv_nsec;
        if (member_store_boot(&server.mstore, fresh, &server.incarnation, err,
                              sizeof(err)) != EFS_OK) {
            fprintf(stderr, "member store refused: %s\n", err);
            member_store_close(&server.mstore);
            return 1;
        }
        struct member_store *ms = &server.mstore;
        if (!join && ms->state == EFS_MEMBER_JOINING) {
            fprintf(stderr, "member store refused: %s was started with --join and is not a "
                    "member yet; start it again with efsd --join [SEED] %s\n", mdraft, storage);
            member_store_close(ms);
            return 1;
        }
        if (ms->state != EFS_MEMBER_JOINING) {
            if (server.port && (server.port != ms->port || strcmp(server.addr, ms->addr))) {
                fprintf(stderr, "member store refused: %s is node %u at %s:%u of cluster "
                        "%016llx; this command names %s:%u (a member keeps its endpoint)\n",
                        mdraft, ms->member_id, ms->addr, ms->port,
                        (unsigned long long)ms->cluster, server.addr, server.port);
                member_store_close(ms);
                return 1;
            }
            server.id = ms->member_id;
            snprintf(server.addr, sizeof(server.addr), "%s", ms->addr);
            server.port = ms->port;
            fprintf(stderr, "node %u: store %s, %s:%u, cluster %016llx, incarnation %llu%s, %s",
                    server.id, mdraft, server.addr, server.port, (unsigned long long)ms->cluster,
                    (unsigned long long)server.incarnation,
                    ms->created ? " (initialized now)" : "", member_store_state_name(ms->state));
            if (ms->state == EFS_MEMBER_FORMED)
                fprintf(stderr, " (%u committed members)\n", ms->members.count);
            else if (ms->state == EFS_MEMBER_PROMISED)
                fprintf(stderr, " (formation proposal of %u members)\n", ms->members.count);
            else
                fprintf(stderr, " (new cluster: forms automatically once two nodes started "
                        "with --join have joined)\n");
        } else {
            fprintf(stderr, "store %s: joining (no node id yet), store %016llx, incarnation "
                    "%llu%s\n", mdraft, (unsigned long long)ms->token,
                    (unsigned long long)server.incarnation,
                    ms->created ? " (initialized now)" : "");
            int jr = join_as_new(&server, seed_host[0] ? seed_host : NULL, seed_port,
                                 listen_ep != NULL);
            /* The rest of startup runs as for any node: a signal before
             * serving ends the process (the serving handler comes later). */
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            if (jr) {
                if (server.listen_fd >= 0)
                    close(server.listen_fd);
                member_store_close(ms);
                return jr == 2 ? 0 : 1;
            }
        }
    }

    server.nodes[0].id = server.id;
    strncpy(server.nodes[0].addr, server.addr, sizeof(server.nodes[0].addr) - 1);
    server.nodes[0].port = server.port;
    server_format_storage_paths(&server, server.nodes[0].storage_path,
                                sizeof(server.nodes[0].storage_path));
    server.nodes[0].quota = server.quota;
    server.nodes[0].used = 0;
    server.nodes[0].token = server.mstore.token;
    server.node_count = 1;

    server_load_nodes(&server);
    if (server.mstore.state == EFS_MEMBER_FORMED)
        server_nodes_keep_members(&server, &server.mstore.members);

    /* Refresh this process's row by id — never smash nodes[0], which may be a peer
     * after load (that bug duplicated one id and dropped another). */
    server_sync_local_membership(&server);
    /* Fail closed before exposing an endpoint. Existing fragment usage is
     * inventoried once per root for both the local counter and safe quota.
     * usage.bin is advisory, so neither result may skip the full inventory. */
    struct timespec inventory_start, inventory_end;
    clock_gettime(CLOCK_MONOTONIC, &inventory_start);
    uint64_t root_usage[EFS_MAX_STORAGE_PATHS], safe_quota;
    if (server_compute_roots_usage(&server, root_usage) != EFS_OK) {
        fprintf(stderr, "Storage quota: cannot inventory storage roots; refusing startup\n");
        return 1;
    }
    uint64_t local_usage = 0;
    for (uint32_t i = 0; i < server.storage_path_count; i++) {
        if (root_usage[i] >= UINT64_MAX - local_usage) {
            fprintf(stderr, "Storage quota: inventory overflow; refusing startup\n");
            return 1;
        }
        local_usage += root_usage[i];
    }
    clock_gettime(CLOCK_MONOTONIC, &inventory_end);
    int64_t inventory_ms = (int64_t)(inventory_end.tv_sec - inventory_start.tv_sec) * 1000 +
                           (inventory_end.tv_nsec - inventory_start.tv_nsec) / 1000000;
    fprintf(stderr, "storage-inventory: roots=%u used=%llu elapsed_ms=%lld\n",
            server.storage_path_count, (unsigned long long)local_usage,
            (long long)inventory_ms);
    if (efs_storage_safe_quota(server.storage_paths, root_usage,
                               server.storage_path_count, &safe_quota)) {
        fprintf(stderr, "Storage quota: cannot establish safe headroom; refusing startup\n");
        return 1;
    }
    const double gib = (double)(UINT64_C(1) << 30);
    const double tib = (double)(UINT64_C(1) << 40);
    const char *maximum_unit = safe_quota >= (UINT64_C(1) << 40) ? "TiB" : "GiB";
    double maximum_scale = safe_quota >= (UINT64_C(1) << 40) ? tib : gib;
    const char *quota_unit = server.quota >= (UINT64_C(1) << 40) ? "TiB" : "GiB";
    double quota_scale = server.quota >= (UINT64_C(1) << 40) ? tib : gib;
    if (server.quota > safe_quota) {
        fprintf(stderr, "Storage quota %.2f %s exceeds safe maximum %.2f %s; refusing startup\n",
                (double)server.quota / quota_scale, quota_unit,
                (double)safe_quota / maximum_scale, maximum_unit);
        return 1;
    }
    int automatic_quota = server.quota == 0;
    if (automatic_quota) {
        server.quota = safe_quota;
        quota_unit = maximum_unit;
        quota_scale = maximum_scale;
    }
    pthread_mutex_lock(&server.lock);
    struct efs_node *local = server_local_node(&server);
    if (!local) {
        pthread_mutex_unlock(&server.lock);
        fprintf(stderr, "Storage quota: local member unavailable; refusing startup\n");
        return 1;
    }
    local->used = local_usage;
    pthread_mutex_unlock(&server.lock);
    server_sync_local_membership(&server);
    server_usage_save(&server);
    fprintf(stderr, "Storage quota: %s %.2f %s (safe maximum %.2f %s; reserve 20%%, minimum 1 GiB per filesystem)\n",
            automatic_quota ? "automatic" : "explicit",
            (double)server.quota / quota_scale, quota_unit,
            (double)safe_quota / maximum_scale, maximum_unit);

    /* W124: the reclaim journal is verified and replayed (fences installed,
     * pending bytes credited against the fresh inventory) before any fragment
     * write can arrive. A damaged journal fails closed, not startup. */
    if (server_reclaim_init(&server) != EFS_OK) {
        fprintf(stderr, "reclaim: cannot initialize reclaim journal; refusing startup\n");
        return 1;
    }

    /* Start listening first. If the configured address/port cannot be bound,
       fail immediately before contacting peers. */
    if (server.listen_fd < 0)
        server.listen_fd = efs_listen_tcp(server.addr, server.port, EFS_LISTEN_BACKLOG);
    if (server.listen_fd < 0) {
        fprintf(stderr, "Failed to listen on %s:%u: %s\n",
                server.addr, server.port, strerror(errno));
        return 1;
    }

    /* SIGTERM may run on a handler rather than interrupt main's accept.
     * Poll bounds flag observation; nonblocking accept cannot outwait it. */
    int listen_flags = fcntl(server.listen_fd, F_GETFL, 0);
    if (listen_flags < 0 || fcntl(server.listen_fd, F_SETFL,
                                  listen_flags | O_NONBLOCK) < 0) {
        perror("nonblocking listener");
        close(server.listen_fd);
        return 1;
    }

    /* K33: whom to say HELLO to. A formed member with persisted peers
     * rejoins through them (the seed may be gone); otherwise the seed it
     * was given, the peer that admitted it, or (promised) the proposal's
     * seed. Until its store learned a committed revision naming it, it
     * keeps saying HELLO. */
    char chost[64] = "";
    uint16_t cport = 0;
    if (seed_host[0]) {
        snprintf(chost, sizeof(chost), "%s", seed_host);
        cport = seed_port;
    } else if (server.rejoin_addr[0]) {
        snprintf(chost, sizeof(chost), "%s", server.rejoin_addr);
        cport = server.rejoin_port;
    } else if (server.mstore.state == EFS_MEMBER_PROMISED && server.id != 1) {
        snprintf(chost, sizeof(chost), "%s", server.mstore.members.m[0].addr);
        cport = server.mstore.members.m[0].port;
    }
    if (chost[0] && (server.mstore.state != EFS_MEMBER_FORMED || server.node_count <= 1)) {
        int joined = server_join_cluster(&server, chost, cport);
        if (joined == EFS_JOIN_REFUSED_FORMED) {
            close(server.listen_fd);
            return 1;
        }
        if (joined == 0)
            server_save_nodes(&server);
        else if (joined != EFS_JOIN_PENDING)
            fprintf(stderr,
                    "Could not reach %s:%u; starting and retrying in background\n", chost, cport);
        if (joined != 0 || !server_raft_host_formed()) {
            snprintf(server.rejoin_addr, sizeof(server.rejoin_addr), "%s", chost);
            server.rejoin_port = cport;
            server_start_rejoin(&server);
        }
    } else if (server.node_count > 1) {
        server.rejoin_addr[0] = '\0';
        if (server_rejoin_cluster(&server) != 0) {
            fprintf(stderr,
                    "Could not rejoin cluster from persisted peers; starting as standalone and retrying in background\n");
            server_start_rejoin(&server);
        } else {
            server_save_nodes(&server);
        }
    } else {
        server_save_nodes(&server);
    }

    /* Do not rebuild 2+1 meta pages on this thread. A large export (tens of
     * thousands of pages) used to block accept() for minutes after a bounce,
     * so joiners never came up and the client timed out every GET. Catch-up
     * rebuilds in the background; GET_CHUNK is served from disk immediately. */

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigint_handler;
        sigemptyset(&sa.sa_mask);
        /* Do not set SA_RESTART: we want accept() to surface EINTR as a
         * fallback if close(listen_fd) races with the syscall entry. */
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
    server_put_sync_serving();
    /* A stop signal caught while joining (join_as_new) still stops. */
    server.running = !__atomic_load_n(&g_stop_signal_ms, __ATOMIC_ACQUIRE);
    if (server_writer_pool_start(&server) != 0) {
        fprintf(stderr, "Failed to start writer pool (%d threads, %u paths)\n",
                server.nwriters, server.storage_path_count);
        return 1;
    }
    server_start_heartbeat(&server);
    if (server_raft_host_start(&server) != 0) {
        fprintf(stderr, "raft-host: start failed\n");
        server_writer_pool_stop(&server);
        return 1;
    }
    if (server_packer_start(&server) != 0) {
        fprintf(stderr, "packer: start failed\n");
        server_raft_host_stop();
        server_writer_pool_stop(&server);
        return 1;
    }
    if (server.discovery_port) {
        if (efs_disc_responder_start(server.discovery_port, discovery_info) != 0)
            fprintf(stderr, "discovery: cannot answer on UDP port %u: %s; joiners must name a "
                    "seed (efsd --join ADDR:PORT STORAGE)\n", server.discovery_port,
                    strerror(errno));
        else
            fprintf(stderr, "discovery: answering on UDP port %u\n", server.discovery_port);
    }

    {
        const char *stripe = (server.storage_path_count > 1) ? "leastq" : "none";
        char storage_disp[EFS_MAX_PATH];
        uint64_t used_disp = 0;
        server_format_storage_paths(&server, storage_disp, sizeof(storage_disp));
        struct efs_node *local = server_local_node(&server);
        if (local)
            used_disp = local->used;
        char ver[256];
        printf("%s, build=%s: node %u listening on %s:%u, storage=%s "
               "(%u paths, stripe=%s), used=%llu, quota=%llu, direct_io=%s, "
               "writers=%d shared\n",
               efs_version_string("efsd", ver, sizeof(ver)), EFS_BUILD_ID,
               server.id, server.addr, server.port, storage_disp,
               server.storage_path_count, stripe,
               (unsigned long long)used_disp,
               (unsigned long long)server.quota,
               server.direct_io ? "on" : "off",
               server.nwriters);
    }
    fflush(stdout);

    if (server.perf) {
        g_perf_pid = efs_start_perf_recorder(getpid(), perf_path);
        if (g_perf_pid < 0) {
            fprintf(stderr, "Warning: could not start perf recorder; continuing without profiling\n");
        }
    }
    if (server.strace) {
        g_strace_pid = efs_start_strace_recorder(getpid(), strace_path);
        if (g_strace_pid < 0)
            fprintf(stderr, "Warning: could not start strace; continuing without it\n");
        else
            fprintf(stderr, "efsd strace pid=%d -> %s\n", (int)g_strace_pid, strace_path);
    }

    while (server.running) {
        struct pollfd listener = { .fd = server.listen_fd, .events = POLLIN };
        int ready = poll(&listener, 1, 250);
        if (!server.running)
            break;
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("listener poll");
            break;
        }
        if (!ready) continue;
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(server.listen_fd, (struct sockaddr *)&addr, &addrlen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) {
                /* SIGINT/SIGTERM clear running. Leaving the loop is
                 * what runs the bounded stop; staying here left efsd
                 * up and the perf file without a footer. */
                if (!server.running)
                    break;
                continue;
            }
            /* EMFILE/ENFILE: do not tear down the accept loop — back off so
             * existing handlers can finish and free fds, then resume. */
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                errno == ENOMEM) {
                fprintf(stderr,
                        "accept: %s (live_conns=%d); backing off\n",
                        strerror(errno), g_live_conns);
                usleep(10 * 1000);
                continue;
            }
            if (errno != EINVAL)
                perror("accept");
            break;
        }

        /* Some platforms inherit the listener's nonblocking flag. Handler
         * framing expects blocking accepted sockets with explicit deadlines. */
        int accepted_flags = fcntl(fd, F_GETFL, 0);
        if (accepted_flags < 0 || fcntl(fd, F_SETFL,
                                       accepted_flags & ~O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }

        int live = __sync_add_and_fetch(&g_live_conns, 1);
        if (live > EFS_SERVER_MAX_CONNS) {
            __sync_fetch_and_sub(&g_live_conns, 1);
            close(fd);
            /* Over-cap: back off like EMFILE so a conn storm doesn't spin. */
            usleep(10 * 1000);
            continue;
        }

        if (!conn_register(fd)) {
            __sync_fetch_and_sub(&g_live_conns, 1);
            close(fd);
            continue;
        }
        pthread_t tid;
        if (efsd_pthread_create(&tid, conn_thread, (void *)(intptr_t)fd) != 0) {
            conn_unregister(fd);
            __sync_fetch_and_sub(&g_live_conns, 1);
            close(fd);
        } else {
            pthread_detach(tid);
        }
    }

    if (server.listen_fd >= 0) {
        close(server.listen_fd);
        server.listen_fd = -1;
    }
    /* The recorders stop in the first teardown phase, inside the deadline. */
    uint64_t stop_t0 = __atomic_load_n(&g_stop_signal_ms, __ATOMIC_ACQUIRE);
    if (!stop_t0)
        stop_t0 = shutdown_now_ms(); /* the accept loop ended without a signal */
    int exit_rc = shutdown_bounded(&server, stop_t0);
    if (!exit_rc && __atomic_load_n(&server.join_refused, __ATOMIC_ACQUIRE))
        exit_rc = 1; /* K33: refused by the cluster, as at the first HELLO */
    pthread_mutex_destroy(&server.lock);
    for (uint32_t i = 0; i < server.export_count; i++)
        efs_export_free(&server.exports[i]);
    return exit_rc;
}
