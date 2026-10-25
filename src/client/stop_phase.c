/* Unmount progress observer (A11 finding 1), see client_internal.h. Kept
 * apart from the connection code so that callers such as the put settle
 * loop link without it. */
#include "client_internal.h"

static void (*g_stop_observer)(const char *, uint64_t, uint64_t);
static __thread int t_stop_owner;

void efs_client_set_stop_observer(void (*fn)(const char *, uint64_t, uint64_t))
{
    __atomic_store_n(&g_stop_observer, fn, __ATOMIC_RELEASE);
    t_stop_owner = fn != NULL;
}

void efs_client_stop_phase(const char *phase, uint64_t step, uint64_t total)
{
    void (*fn)(const char *, uint64_t, uint64_t) =
        __atomic_load_n(&g_stop_observer, __ATOMIC_ACQUIRE);
    if (fn)
        fn(phase, step, total);
}

int efs_client_stop_owner(void)
{
    return t_stop_owner;
}
