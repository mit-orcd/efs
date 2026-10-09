#include "efs/batch_obs.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int efs_batch_obs_on;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static uint64_t counters[BO_COUNTER_N], rejected[BO_COUNTER_N], last_dump;
/* Bin 0 is exactly zero; bin 1 exactly one; subsequent bins are powers of
 * two with inclusive upper bounds, last bin overflow. Fixed resident memory. */
#define BINS 25
static struct { uint64_t n, sum, max, bins[BINS]; } hist[BH_HIST_N];
static const char *cn[BO_COUNTER_N] = {
    "sync", "sync_error", "durable_skip", "hold_skip", "hold_open", "hold_close", "rotate",
    "eligible", "reject_store", "reject_role", "reject_type", "reject_route", "reject_term",
    "reject_empty", "reject_entries", "reject_log", "reject_tail", "reject_config", "reject_admit", "reject_bytes",
    "follow_end", "follow_ineligible", "follow_bytes", "follow_error",
    "host_end", "host_group", "host_count", "host_bytes", "inbox_empty", "spin_cap", "tail_behind", "tail_ahead", "tail_term", "inbox_drop",
    "ae_initial", "ae_extend", "ae_timeout", "ae_other",
    "reply_partial", "reply_covered", "drop_data", "drop_empty",
    "drop_other", "accept_data", "accept_empty"
};
static const char *hn[BH_HIST_N] = {
    "wal_entries", "wal_bytes", "sync_us", "sync_wait_us", "sync_waiters",
    "hold_us", "hold_depth", "proposers", "inbox", "host_messages",
    "follow_messages", "follow_entries", "follow_assembly_us", "follow_total_us", "spins"
};
static void init_once(void)
{
    const char *s = getenv("EFS_BATCH_OBS");
    __atomic_store_n(&efs_batch_obs_on, s && !strcmp(s,"1"), __ATOMIC_RELEASE);
}
void efs_batch_obs_init(void) { pthread_once(&once, init_once); }
uint64_t efs_batch_obs_now_us(void)
{
    struct timespec t = {0,0};
    int saved = errno;
    clock_gettime(CLOCK_MONOTONIC, &t);
    errno = saved;
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void efs_batch_obs_count(enum efs_batch_counter key)
{
    __atomic_add_fetch(&counters[key], 1, __ATOMIC_RELAXED);
}
void efs_batch_obs_reject(enum efs_batch_counter key)
{
    __atomic_add_fetch(&rejected[key], 1, __ATOMIC_RELAXED);
}
void efs_batch_obs_hist(enum efs_batch_hist key, uint64_t value)
{
    unsigned bin = 0;
    uint64_t upper = 1, old;
    if (value) {
        bin = 1;
        while (value > upper && bin < BINS-1) { upper <<= 1; bin++; }
    }
    __atomic_add_fetch(&hist[key].n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hist[key].sum, value, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hist[key].bins[bin], 1, __ATOMIC_RELAXED);
    old = __atomic_load_n(&hist[key].max, __ATOMIC_RELAXED);
    while (old < value && !__atomic_compare_exchange_n(&hist[key].max, &old, value, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
void efs_batch_obs_dump(uint64_t now_us)
{
    unsigned i, b;
    int saved = errno;
    if (!BO_ON() || (last_dump && now_us - last_dump < 5000000)) return;
    /* One pump owns dump cadence. Cumulative atomics are not a transactional
     * snapshot: interval boundary counts can differ by concurrent updates. */
    last_dump = now_us;
    flockfile(stderr);
    fprintf(stderr,"batch-obs {\"mono_us\":%llu,\"counters\":{",(unsigned long long)now_us);
    for (i=0;i<BO_COUNTER_N;i++)
        fprintf(stderr,"%s\"%s\":%llu",i?",":"",cn[i],(unsigned long long)__atomic_load_n(&counters[i],__ATOMIC_RELAXED));
    fprintf(stderr,"},\"follow_reject\":{");
    for (i=0;i<BO_COUNTER_N;i++)
        fprintf(stderr,"%s\"%s\":%llu",i?",":"",cn[i],(unsigned long long)__atomic_load_n(&rejected[i],__ATOMIC_RELAXED));
    fprintf(stderr,"},\"hist\":{");
    for (i=0;i<BH_HIST_N;i++) {
        fprintf(stderr,"%s\"%s\":{\"n\":%llu,\"sum\":%llu,\"max\":%llu,\"bins\":[",i?",":"",hn[i],
            (unsigned long long)__atomic_load_n(&hist[i].n,__ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hist[i].sum,__ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hist[i].max,__ATOMIC_RELAXED));
        for (b=0;b<BINS;b++) fprintf(stderr,"%s%llu",b?",":"",(unsigned long long)__atomic_load_n(&hist[i].bins[b],__ATOMIC_RELAXED));
        fprintf(stderr,"]}");
    }
    fprintf(stderr,"}}\n");
    funlockfile(stderr);
    errno = saved;
}
