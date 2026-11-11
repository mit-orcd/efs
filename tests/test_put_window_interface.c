/* Task U1: the PUT window through its owner interface (put_window.h).
 *
 * Links the real src/client/put_window.c and nothing else of the client; the
 * only symbol it takes from elsewhere is efs_client_rpc_deadline_ms, defined
 * here as a settable stub. Each case leaves every window closed.
 *
 * Cases: open then close leaves the window closed; two opens need two
 * closes; windows of other inodes do not block a wait; the wait returns at
 * once when nothing is open; a close wakes a blocked wait; the wait gives up
 * at its own bound and at the caller's RPC deadline and releases the mutex;
 * the reader wait is the same wait with an 8 s bound; more inodes than slots
 * (the overflow count makes every inode busy until it is closed); threads
 * opening and closing concurrently end closed (TSan entry). */
#include "put_window.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rpc_deadline; /* 0 = none */

uint64_t efs_client_rpc_deadline_ms(void) { return __atomic_load_n(&rpc_deadline, __ATOMIC_ACQUIRE); }

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void test_open_close_closes(void)
{
    put_win_open(5);
    assert(put_win_wait(5, 20) == EFS_ERR_BUSY);
    put_win_close(5);
    assert(put_win_wait(5, 20) == 0);
}

static void test_two_opens_need_two_closes(void)
{
    put_win_open(5);
    put_win_open(5);
    put_win_close(5);
    assert(put_win_wait(5, 20) == EFS_ERR_BUSY);
    put_win_close(5);
    assert(put_win_wait(5, 20) == 0);
}

static void test_other_inode_not_blocked(void)
{
    put_win_open(5);
    assert(put_win_wait(6, 20) == 0);
    put_win_close(5);
}

static void test_wait_returns_at_once_when_closed(void)
{
    uint64_t t0 = now_ms();

    assert(put_win_wait(7, 8000) == 0);
    assert(efs_dcache_put_win_wait(7) == 0);
    assert(now_ms() - t0 < 1000);
}

struct waiter {
    int rc;
    int started;
    uint64_t t0, t1;
};

static void *wait_thread(void *p)
{
    struct waiter *w = p;

    w->t0 = now_ms();
    __atomic_store_n(&w->started, 1, __ATOMIC_RELEASE);
    w->rc = put_win_wait(9, 5000);
    w->t1 = now_ms();
    return NULL;
}

static void test_close_wakes_wait(void)
{
    struct waiter w = {0};
    pthread_t t;

    put_win_open(9);
    assert(!pthread_create(&t, NULL, wait_thread, &w));
    while (!__atomic_load_n(&w.started, __ATOMIC_ACQUIRE))
        sched_yield();
    for (uint64_t s = now_ms(); now_ms() - s < 20;) /* let it block: busy-wait, no sleep */
        sched_yield();
    put_win_close(9);
    assert(!pthread_join(t, NULL));
    assert(w.rc == 1);
    assert(w.t1 - w.t0 < 4000);
}

static void test_wait_gives_up_at_its_bound(void)
{
    uint64_t t0 = now_ms(), dt;

    put_win_open(11);
    assert(put_win_wait(11, 40) == EFS_ERR_BUSY);
    dt = now_ms() - t0;
    assert(dt >= 40 && dt < 1000);
    /* the mutex was released: open and close from this thread do not block */
    put_win_open(12);
    put_win_close(12);
    put_win_close(11);
    assert(put_win_wait(11, 20) == 0);
}

static void test_rpc_deadline_caps_the_wait(void)
{
    uint64_t t0 = now_ms();

    put_win_open(13);
    __atomic_store_n(&rpc_deadline, t0 + 30, __ATOMIC_RELEASE);
    assert(put_win_wait(13, 8000) == EFS_ERR_BUSY);
    assert(now_ms() - t0 < 1000);
    __atomic_store_n(&rpc_deadline, now_ms(), __ATOMIC_RELEASE); /* already past */
    assert(put_win_wait(13, 8000) == EFS_ERR_BUSY);
    __atomic_store_n(&rpc_deadline, 0, __ATOMIC_RELEASE);
    put_win_close(13);
    assert(put_win_wait(13, 8000) == 0);
}

static void test_reader_wait(void)
{
    uint64_t t0 = now_ms();

    assert(efs_dcache_put_win_wait(15) == 0); /* none open anywhere */
    put_win_open(16);
    assert(efs_dcache_put_win_wait(15) == 0); /* another inode's window */
    put_win_open(15);
    __atomic_store_n(&rpc_deadline, t0 + 30, __ATOMIC_RELEASE); /* the 8 s bound is capped */
    assert(efs_dcache_put_win_wait(15) == EFS_ERR_BUSY);
    __atomic_store_n(&rpc_deadline, 0, __ATOMIC_RELEASE);
    put_win_close(15);
    put_win_close(16);
    assert(efs_dcache_put_win_wait(15) == 0);
}

#define PW_SLOTS 512 /* put_window.c: PUT_WIN_SLOTS */

static void test_more_inodes_than_slots(void)
{
    efs_ino_t i;

    for (i = 1000; i < 1000 + PW_SLOTS + 1; i++)
        put_win_open(i);
    /* the last inode found no slot: the overflow count makes every inode busy */
    assert(put_win_wait(99999, 20) == EFS_ERR_BUSY);
    assert(put_win_wait(1000, 20) == EFS_ERR_BUSY);
    put_win_close(1000 + PW_SLOTS); /* no slot of its own: ends the overflow window */
    assert(put_win_wait(99999, 20) == 0);
    assert(put_win_wait(1000, 20) == EFS_ERR_BUSY);
    for (i = 1000; i < 1000 + PW_SLOTS; i++)
        put_win_close(i);
    for (i = 1000; i < 1000 + PW_SLOTS + 1; i++)
        assert(put_win_wait(i, 20) == 0);
}

#define CHURN_THREADS 4
#define CHURN_ROUNDS 20000

static void *churn(void *arg)
{
    uintptr_t id = (uintptr_t)arg;

    for (int r = 0; r < CHURN_ROUNDS; r++) {
        efs_ino_t ino = 2000 + (efs_ino_t)((id + (uintptr_t)r) % 8);

        put_win_open(ino);
        if (r % 64 == 0)
            (void)put_win_wait(ino + 100, 1);
        put_win_close(ino);
    }
    return NULL;
}

static void test_concurrent_open_close_ends_closed(void)
{
    pthread_t t[CHURN_THREADS];

    for (uintptr_t i = 0; i < CHURN_THREADS; i++)
        assert(!pthread_create(&t[i], NULL, churn, (void *)i));
    for (int i = 0; i < CHURN_THREADS; i++)
        assert(!pthread_join(t[i], NULL));
    for (efs_ino_t ino = 2000; ino < 2008; ino++)
        assert(put_win_wait(ino, 1000) == 0);
    assert(efs_dcache_put_win_wait(2000) == 0);
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
    RUN(test_open_close_closes);
    RUN(test_two_opens_need_two_closes);
    RUN(test_other_inode_not_blocked);
    RUN(test_wait_returns_at_once_when_closed);
    RUN(test_close_wakes_wait);
    RUN(test_wait_gives_up_at_its_bound);
    RUN(test_rpc_deadline_caps_the_wait);
    RUN(test_reader_wait);
    RUN(test_more_inodes_than_slots);
    RUN(test_concurrent_open_close_ends_closed);
    if (g_list)
        return 0;
    for (int i = 1; i < argc; i++)
        if (!g_used[i]) {
            fprintf(stderr, "%s: no case matches '%s' (see --list)\n", argv[0], argv[i]);
            bad = 1;
        }
    if (bad)
        return 2;
    puts("put_window interface: open/close, counted opens, other inodes, immediate return, "
         "close wake, own bound, RPC deadline, reader wait, overflow, concurrent churn PASS");
    return 0;
}
