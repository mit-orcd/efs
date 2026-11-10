/* Task DM: the write-back pool through its owner interface (writeback.h).
 *
 * Links the real src/client/efs_fuse/writeback.c and nothing else of the
 * client. The module offers no PUT hook, so the PUT path and the few
 * helpers it calls from other files are link-time definitions here:
 * efs_client_write_no_replicate (a gate the test opens), the thread-local
 * RPC deadline, wb_lock_budget (open.c's trylock-until-deadline rule),
 * fuse_chunk_size, stats_now_ms, the lookup-memo hooks and efs_strerror.
 *
 * Cases: drains and pending before the pool starts; start, stop and
 * restart; the sticky error set by a failed PUT, kept by another inode's
 * drain, returned once by its own inode's drain and by the global drain;
 * pending per inode and overall with a blocked PUT, drains cut at the
 * caller's deadline that leave the work and the error where they were;
 * a drain waiting when the workers are stopped; Task DT: a worker start
 * that fails with another writer queued behind it (built with
 * -Wl,--wrap=pthread_create), and drains and pending racing start/stop
 * cycles and a second stopper. */
#include "efs_fuse/writeback.h"
#include "efs_fuse/support.h"
#include "efs_fuse/getattr.h"
#include "efs_fuse/open.h"
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread uint64_t t_deadline;
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static efs_ino_t block_ino, fail_ino;
static int gate_open, gate_rc, entered;
static unsigned put_calls, memo_begin, memo_end;

/* Fail the pthread_create call numbered fail_at (1-based) once armed. */
int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
static int fail_at, create_calls;
static pthread_t racer;
static int racer_rc = -999;
static void *racer_run(void *arg);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a,
                          void *(*fn)(void *), void *arg)
{
    if (!__atomic_load_n(&fail_at, __ATOMIC_ACQUIRE) ||
        __atomic_add_fetch(&create_calls, 1, __ATOMIC_ACQ_REL) != fail_at)
        return __real_pthread_create(t, a, fn, arg);
    __atomic_store_n(&fail_at, 0, __ATOMIC_RELEASE);
    /* A second writer arrives while this start is being undone. */
    assert(!__real_pthread_create(&racer, NULL, racer_run, NULL));
    return EAGAIN;
}

uint64_t stats_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
uint64_t efs_client_rpc_deadline_ms(void) { return t_deadline; }
void efs_client_rpc_set_deadline_ms(uint64_t ms) { t_deadline = ms; }
int efs_client_rpc_past_deadline(void)
{
    return t_deadline && stats_now_ms() >= t_deadline;
}
int wb_lock_budget(pthread_mutex_t *mu)
{
    for (;;) {
        if (!t_deadline)
            return pthread_mutex_lock(mu) == 0 ? EFS_OK : EFS_ERR_IO;
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        if (!pthread_mutex_trylock(mu))
            return EFS_OK;
        sched_yield();
    }
}
uint32_t fuse_chunk_size(void) { return 131072; }
void lookup_memo_mutation_begin(void) { __atomic_add_fetch(&memo_begin, 1, __ATOMIC_RELAXED); }
void lookup_memo_mutation_end(void) { __atomic_add_fetch(&memo_end, 1, __ATOMIC_RELAXED); }
const char *efs_strerror(int rc) { (void)rc; return "error"; }

int efs_client_write_no_replicate(efs_ino_t ino, uint64_t offset, size_t size,
                                  const char *buf)
{
    (void)offset; (void)size;
    assert(buf[0] == 'X' && t_deadline);
    pthread_mutex_lock(&gate_mu);
    put_calls++;
    int rc = ino == fail_ino ? EFS_ERR_IO : EFS_OK;
    if (ino == block_ino) {
        entered = 1;
        pthread_cond_broadcast(&gate_cv);
        while (!gate_open)
            pthread_cond_wait(&gate_cv, &gate_mu);
        rc = gate_rc;
    }
    pthread_mutex_unlock(&gate_mu);
    return rc;
}

static int write_one(efs_ino_t ino)
{
    char *p = malloc(131072);
    assert(p);
    p[0] = 'X';
    return efs_wb_enqueue_owned(ino, 0, 131072, p, NULL, 0);
}

struct writer { efs_ino_t ino; int rc; };
static void *writer_run(void *arg)
{
    struct writer *w = arg;
    w->rc = write_one(w->ino);
    return NULL;
}
static void *racer_run(void *arg)
{
    (void)arg;
    racer_rc = write_one(12);
    return NULL;
}
static int cycles_done;
static void *reader_run(void *arg)
{
    efs_ino_t ino = (efs_ino_t)(intptr_t)arg;
    while (!__atomic_load_n(&cycles_done, __ATOMIC_ACQUIRE)) {
        assert(efs_wb_pending(0) >= 0 && efs_wb_pending(ino) >= 0);
        assert(efs_wb_sync_ino(ino) == EFS_OK && efs_wb_sync() == EFS_OK);
    }
    return NULL;
}
static void *stopper_run(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&cycles_done, __ATOMIC_ACQUIRE))
        efs_wb_stop();
    return NULL;
}
static void *sync_run(void *arg)
{
    *(int *)arg = efs_wb_sync();
    return NULL;
}
static void *stop_run(void *arg)
{
    (void)arg;
    efs_wb_stop();
    return NULL;
}

/* Start a writer whose PUT blocks at the gate; return once it is in flight. */
static void block_put(pthread_t *t, struct writer *w, efs_ino_t ino)
{
    pthread_mutex_lock(&gate_mu);
    block_ino = ino; gate_open = 0; gate_rc = EFS_OK; entered = 0;
    pthread_mutex_unlock(&gate_mu);
    w->ino = ino;
    assert(!pthread_create(t, NULL, writer_run, w));
    pthread_mutex_lock(&gate_mu);
    while (!entered)
        pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
}
static void open_gate(int rc)
{
    pthread_mutex_lock(&gate_mu);
    gate_rc = rc; gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
}

/* Case filter: no argument runs every case; an argument runs the cases whose
 * name contains it; --list prints the names; an argument that matches no case
 * exits 2, so a misspelling is not a silent pass. */
static int g_argc, g_list;
static char **g_argv;
static char *g_used;

static void run_init(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
    g_list = argc == 2 && !strcmp(argv[1], "--list");
    g_used = calloc((size_t)argc, 1);
    if (!g_used)
        abort();
}

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

static int run_unmatched(void)
{
    int bad = 0;

    for (int i = 1; i < g_argc && !g_list; i++)
        if (!g_used[i]) {
            fprintf(stderr, "%s: no case matches '%s' (see --list)\n", g_argv[0], g_argv[i]);
            bad = 1;
        }
    return bad;
}

static void test_unstarted(void)
{
    /* Unstarted pool: nothing pending, drains succeed, stop is a no-op. */
    assert(efs_wb_pending(0) == 0 && efs_wb_pending(7) == 0);
    assert(efs_wb_sync() == EFS_OK && efs_wb_sync_ino(7) == EFS_OK);
    efs_wb_stop();
}

static void test_start_stop_restart(void)
{
    /* Start on first enqueue, stop, restart. */
    assert(write_one(1) == EFS_OK && put_calls == 1);
    assert(efs_wb_pending(0) == 0 && efs_wb_pending(1) == 0);
    efs_wb_stop();
    assert(efs_wb_pending(0) == 0 && efs_wb_sync() == EFS_OK);
    assert(write_one(1) == EFS_OK && put_calls == 2);
}

static void test_sticky_error(void)
{
    /* Sticky error: set by ino 42, kept by ino 7's drain, returned once. */
    fail_ino = 42;
    assert(write_one(42) == EFS_ERR_IO);
    assert(write_one(7) == EFS_OK); /* another inode is not refused */
    assert(efs_wb_sync_ino(7) == EFS_OK);
    assert(efs_wb_sync_ino(42) == EFS_ERR_IO && efs_wb_sync_ino(42) == EFS_OK);
    assert(write_one(42) == EFS_ERR_IO);
    assert(efs_wb_sync() == EFS_ERR_IO && efs_wb_sync() == EFS_OK);
    fail_ino = 0;
}

static void test_deadline_drains(void)
{
    /* Pending and drains bounded by the caller's deadline. */
    pthread_t t;
    struct writer w;
    block_put(&t, &w, 5);
    assert(efs_wb_pending(5) == 1 && efs_wb_pending(6) == 0);
    assert(efs_wb_pending(0) == 1);
    for (int per = 0; per < 2; per++) {
        uint64_t start = stats_now_ms();
        t_deadline = start + 20;
        assert((per ? efs_wb_sync_ino(5) : efs_wb_sync()) == EFS_ERR_BUSY);
        assert(stats_now_ms() - start < 500);
        assert(efs_wb_pending(5) == 1 && efs_wb_pending(0) == 1);
    }
    t_deadline = stats_now_ms() + 20; /* other inode: nothing to wait for */
    assert(efs_wb_sync_ino(6) == EFS_OK);
    t_deadline = 0;
    open_gate(EFS_ERR_IO);
    assert(!pthread_join(t, NULL) && w.rc == EFS_ERR_IO);
    assert(efs_wb_pending(0) == 0);
    t_deadline = stats_now_ms() + 20; /* the error survived the BUSY drains */
    assert(efs_wb_sync_ino(5) == EFS_ERR_IO);
    t_deadline = 0;
}

static void test_drain_across_stop(void)
{
    /* A drain waiting when the workers are stopped: the in-flight PUT ends,
     * the stop joins after it, the drain returns its error, the pool is
     * unstarted again. */
    pthread_t t, ts, tstop;
    struct writer w;
    int sync_rc = -999;
    block_put(&t, &w, 9);
    assert(!pthread_create(&ts, NULL, sync_run, &sync_rc));
    assert(!pthread_create(&tstop, NULL, stop_run, NULL));
    unsigned before = put_calls;
    open_gate(EFS_ERR_IO);
    assert(!pthread_join(tstop, NULL)); /* returns only after the PUT */
    assert(!pthread_join(t, NULL) && w.rc == EFS_ERR_IO && put_calls == before);
    assert(!pthread_join(ts, NULL));
    assert(efs_wb_pending(0) == 0 && efs_wb_pending(9) == 0);
    /* The drain either saw the failed PUT (EFS_ERR_IO) or ran after the stop
     * reset the pool and found it unstarted; the stop does not consume the
     * error, so then the restarted pool's drain still returns it. */
    assert(sync_rc == EFS_ERR_IO || sync_rc == EFS_OK);
    assert(write_one(1) == EFS_OK);
    assert(efs_wb_sync() == (sync_rc == EFS_OK ? EFS_ERR_IO : EFS_OK));
    efs_wb_stop();
}

static void test_failed_start(void)
{
    /* Task DT: the second worker start fails; the first is joined, the
     * writer gets NOMEM with its buffer released, and a writer racing the
     * undo gets IO (pool stopping) or starts a fresh pool, never a pool
     * whose workers[] is being joined. */
    unsigned before_fail = put_calls;
    __atomic_store_n(&create_calls, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&fail_at, 2, __ATOMIC_RELEASE);
    assert(write_one(11) == EFS_ERR_NOMEM);
    assert(!pthread_join(racer, NULL));
    assert(racer_rc == EFS_OK || racer_rc == EFS_ERR_IO);
    assert(put_calls == before_fail + (racer_rc == EFS_OK));
    efs_wb_stop();
    assert(efs_wb_pending(0) == 0 && efs_wb_sync() == EFS_OK);
    assert(write_one(1) == EFS_OK);
    efs_wb_stop();
}

static void test_drains_race_stop(void)
{
    /* Task DT: drains and pending race start/stop cycles and a second
     * stopper; an enqueue racing a stop gets IO, otherwise its PUT rc. */
    pthread_t readers[4], stopper;
    for (int i = 0; i < 4; i++)
        assert(!pthread_create(&readers[i], NULL, reader_run,
                               (void *)(intptr_t)(3 + i)));
    assert(!pthread_create(&stopper, NULL, stopper_run, NULL));
    for (int round = 0; round < 50; round++) {
        int rc = write_one(3 + round % 4);
        assert(rc == EFS_OK || rc == EFS_ERR_IO);
        efs_wb_stop();
    }
    __atomic_store_n(&cycles_done, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 4; i++)
        assert(!pthread_join(readers[i], NULL));
    assert(!pthread_join(stopper, NULL));
    efs_wb_stop();
    assert(efs_wb_pending(0) == 0 && efs_wb_sync() == EFS_OK);
}

static void test_memo_balance(void)
{
    assert(memo_begin == put_calls && memo_end == put_calls);
}

int main(int argc, char **argv)
{
    run_init(argc, argv);
    RUN(test_unstarted);
    RUN(test_start_stop_restart);
    RUN(test_sticky_error);
    RUN(test_deadline_drains);
    RUN(test_drain_across_stop);
    RUN(test_failed_start);
    RUN(test_drains_race_stop);
    RUN(test_memo_balance);
    if (g_list)
        return 0;
    if (run_unmatched())
        return 2;
    puts("wb interface: unstarted, start/stop/restart, sticky error, deadline drains, drain across stop, failed start, drains racing stop PASS");
    return 0;
}
