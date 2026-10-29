/* Task AI measurement ONLY. Inject into a disposable rdma.c copy, never the
 * shipping source. All registry critical sections stay at their original
 * boundaries. Per-thread bounded counters are updated AFTER unlocking.
 * Timing includes clock/lock-call overhead; compare matched uninstrumented
 * runs before treating this as a production contention estimate. */
#include <inttypes.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#define AI_REG_THREADS 1024
struct ai_reg_row {
    uint64_t seq, calls, wait_ns, hold_ns, max_wait_ns, max_hold_ns, first_ns, last_ns;
    uint64_t tid;
};
static struct ai_reg_row ai_rows[AI_REG_THREADS][AI_REG_SITES];
static unsigned ai_next_thread, ai_overflow;
static __thread unsigned ai_thread_slot;
static __thread int ai_registered;
static __thread struct ai_reg_row *ai_active;
static __thread uint64_t ai_before, ai_acquired;
static uint64_t ai_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static struct ai_reg_row *ai_row(unsigned site)
{
    if (!ai_registered) {
        ai_thread_slot = __atomic_fetch_add(&ai_next_thread, 1, __ATOMIC_RELAXED);
        ai_registered = 1;
        if (ai_thread_slot >= AI_REG_THREADS)
            __atomic_store_n(&ai_overflow, 1, __ATOMIC_RELAXED);
    }
    if (ai_thread_slot >= AI_REG_THREADS) return NULL;
    return &ai_rows[ai_thread_slot][site];
}
static void ai_registry_lock(unsigned site)
{
    if (ai_active) abort(); /* the original mutex is non-recursive */
    struct ai_reg_row *r = ai_row(site);
    if (r) __atomic_add_fetch(&r->seq, 1, __ATOMIC_RELEASE);
    ai_before = ai_now();
    int rc = pthread_mutex_lock(&g_reg_lock);
    ai_acquired = ai_now();
    if (rc) abort();
    ai_active = r;
}
#define AI_GET(r, f) __atomic_load_n(&(r)->f, __ATOMIC_RELAXED)
#define AI_SET(r, f, v) __atomic_store_n(&(r)->f, (v), __ATOMIC_RELAXED)
static void ai_registry_unlock(void)
{
    struct ai_reg_row *r = ai_active;
    uint64_t end = ai_now();
    int rc = pthread_mutex_unlock(&g_reg_lock);
    ai_active = NULL;
    if (rc) abort();
    if (!r) return; /* overflow is reported, never hidden */
    uint64_t wait = ai_acquired - ai_before, hold = end - ai_acquired;
    if (!AI_GET(r, calls)) {
        AI_SET(r, first_ns, ai_before);
#ifdef __linux__
        AI_SET(r, tid, (uint64_t)syscall(SYS_gettid));
#else
        AI_SET(r, tid, (uint64_t)(uintptr_t)pthread_self());
#endif
    }
    AI_SET(r, calls, AI_GET(r, calls) + 1);
    AI_SET(r, wait_ns, AI_GET(r, wait_ns) + wait);
    AI_SET(r, hold_ns, AI_GET(r, hold_ns) + hold);
    if (wait > AI_GET(r, max_wait_ns)) AI_SET(r, max_wait_ns, wait);
    if (hold > AI_GET(r, max_hold_ns)) AI_SET(r, max_hold_ns, hold);
    AI_SET(r, last_ns, end);
    __atomic_add_fetch(&r->seq, 1, __ATOMIC_RELEASE);
}
static void __attribute__((destructor)) ai_registry_dump(void)
{
    fprintf(stderr, "AI_REGISTRY_META {\"pid\":%ld,\"overflow\":%u,\"thread_slots\":%u,\"instrumented\":true}\n",
            (long)getpid(), __atomic_load_n(&ai_overflow, __ATOMIC_RELAXED),
            __atomic_load_n(&ai_next_thread, __ATOMIC_RELAXED));
    for (unsigned t = 0; t < AI_REG_THREADS; t++) {
        for (unsigned s = 0; s < AI_REG_SITES; s++) {
            struct ai_reg_row *r = &ai_rows[t][s];
            uint64_t seq = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
            uint64_t calls=AI_GET(r,calls), wait=AI_GET(r,wait_ns), hold=AI_GET(r,hold_ns);
            uint64_t mw=AI_GET(r,max_wait_ns), mh=AI_GET(r,max_hold_ns);
            uint64_t first=AI_GET(r,first_ns), last=AI_GET(r,last_ns), tid=AI_GET(r,tid);
            unsigned incomplete = (seq & 1) || seq != __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
            if (!calls && !incomplete) continue;
            fprintf(stderr, "AI_REGISTRY {\"pid\":%ld,\"tid\":%" PRIu64 ",\"function\":\"%s\",\"calls\":%" PRIu64
                    ",\"wait_ns\":%" PRIu64 ",\"hold_ns\":%" PRIu64 ",\"max_wait_ns\":%" PRIu64
                    ",\"max_hold_ns\":%" PRIu64 ",\"first_ns\":%" PRIu64 ",\"last_ns\":%" PRIu64 ",\"incomplete\":%u}\n",
                    (long)getpid(),tid,ai_site_names[s],calls,wait,hold,mw,mh,first,last,incomplete);
        }
    }
}
