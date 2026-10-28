/* Measurement-export overlay only. Never included by product builds.
 * Identical bounded logarithmic histograms in both A/B builds. Dispatch
 * means harvest processing, not NIC-to-userspace completion latency. */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static uint64_t ac_hist[6][64], ac_max[6];
static const char *ac_names[] = {"registry_poll_wait_ns", "registry_totals_wait_ns",
    "registry_lifetime_wait_ns", "poll_dispatch_ns", "pump_work_ns", "dump_ns"};
static uint64_t ac_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
static void ac_record(unsigned kind, uint64_t n) {
    unsigned b=n ? 63u-(unsigned)__builtin_clzll(n) : 0;
    __atomic_fetch_add(&ac_hist[kind][b], 1, __ATOMIC_RELAXED);
    uint64_t old=__atomic_load_n(&ac_max[kind],__ATOMIC_RELAXED);
    while(n>old && !__atomic_compare_exchange_n(&ac_max[kind],&old,n,1,
          __ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
}
__attribute__((destructor)) static void ac_finish(void) {
    for(unsigned k=0;k<6;k++) {
        uint64_t n=0; for(unsigned b=0;b<64;b++)n+=ac_hist[k][b];
        if(!n)continue;
        fprintf(stderr,"ac-probe: scope=%s metric=%s count=%llu max=%llu bins=",
                AC_SCOPE,ac_names[k],(unsigned long long)n,(unsigned long long)ac_max[k]);
        for(unsigned b=0;b<64;b++)fprintf(stderr,"%s%llu",b?",":"",(unsigned long long)ac_hist[k][b]);
        fputc('\n',stderr);
    }
}
