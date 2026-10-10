#ifndef EFS_QUERY_BUILD_TIME_H
#define EFS_QUERY_BUILD_TIME_H
#include <stdint.h>
#include <time.h>
/* Diagnostic wall-clock phases only; never used for admission or freshness. */
static inline uint64_t query_build_clock(void)
{
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC,&t) ? 0 :
        (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static inline uint64_t query_build_elapsed(uint64_t start)
{
    uint64_t end=query_build_clock();
    return start && end>=start ? end-start : 0;
}
#endif
