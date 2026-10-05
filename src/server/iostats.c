#include "server_internal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Always-on data-plane I/O stats, same diagnostic style as the raft-obs
 * counters: __ATOMIC_RELAXED __atomic ops on plain u64s, torn reads
 * acceptable, no locks or allocation on the hot path — two
 * clock_gettime(CLOCK_MONOTONIC) calls and a few fetch_adds per op
 * (read_prof_add in raft_host.c is the template). */

#define IOSTAT_RING 64

struct iostat_class {
    uint64_t ops;
    uint64_t bytes;
    uint64_t errors;
    uint64_t us_sum;
    uint64_t us_max;
    /* Last-64 latency ring (us); ring_n counts total samples. */
    uint64_t ring[IOSTAT_RING];
    uint64_t ring_n;
};

static struct iostat_class g_cls[EFS_IO_STATS_CLASSES];
static uint64_t g_start_us;
static uint64_t g_last_dump_us; /* pump thread only */

uint64_t efs_iostats_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

void efs_iostats_add(int cls, uint64_t bytes, uint64_t us, int err)
{
    struct iostat_class *c;
    uint64_t m, n, z;

    if (cls < 0 || cls >= EFS_IO_STATS_CLASSES)
        return;
    z = 0;
    if (!__atomic_load_n(&g_start_us, __ATOMIC_RELAXED))
        __atomic_compare_exchange_n(&g_start_us, &z, efs_iostats_now_us(), 1,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    c = &g_cls[cls];
    __atomic_fetch_add(&c->ops, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&c->bytes, bytes, __ATOMIC_RELAXED);
    if (err)
        __atomic_fetch_add(&c->errors, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&c->us_sum, us, __ATOMIC_RELAXED);
    m = __atomic_load_n(&c->us_max, __ATOMIC_RELAXED);
    while (us > m &&
           !__atomic_compare_exchange_n(&c->us_max, &m, us, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
    n = __atomic_fetch_add(&c->ring_n, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&c->ring[n % IOSTAT_RING], us, __ATOMIC_RELAXED);
}

/* Median of the last IOSTAT_RING samples (insertion sort, as in
 * host_obs_dump). 0 when nothing was recorded. */
static uint64_t class_p50(const struct iostat_class *c)
{
    uint64_t n = __atomic_load_n(&c->ring_n, __ATOMIC_RELAXED);
    uint64_t samp[IOSTAT_RING];
    uint32_t cnt, i, j;

    if (!n)
        return 0;
    cnt = n < IOSTAT_RING ? (uint32_t)n : IOSTAT_RING;
    for (i = 0; i < cnt; i++)
        samp[i] = __atomic_load_n(&c->ring[(n - cnt + i) % IOSTAT_RING],
                                  __ATOMIC_RELAXED);
    for (i = 1; i < cnt; i++) {
        uint64_t v = samp[i];

        j = i;
        while (j > 0 && samp[j - 1] > v) {
            samp[j] = samp[j - 1];
            j--;
        }
        samp[j] = v;
    }
    return samp[cnt / 2];
}

void efs_iostats_snapshot(struct efs_msg_io_stats_reply *out)
{
    uint64_t start = __atomic_load_n(&g_start_us, __ATOMIC_RELAXED);
    int i;

    memset(out, 0, sizeof(*out));
    out->uptime_us = start ? efs_iostats_now_us() - start : 0;
    for (i = 0; i < EFS_IO_STATS_CLASSES; i++) {
        const struct iostat_class *c = &g_cls[i];

        out->cls[i].ops = __atomic_load_n(&c->ops, __ATOMIC_RELAXED);
        out->cls[i].bytes = __atomic_load_n(&c->bytes, __ATOMIC_RELAXED);
        out->cls[i].errors = __atomic_load_n(&c->errors, __ATOMIC_RELAXED);
        out->cls[i].us_sum = __atomic_load_n(&c->us_sum, __ATOMIC_RELAXED);
        out->cls[i].us_max = __atomic_load_n(&c->us_max, __ATOMIC_RELAXED);
        out->cls[i].p50_us = class_p50(c);
    }
}

/* Called from the raft host pump; prints one line per ~5 s once any class
 * has seen an op. */
void efs_iostats_dump(int force)
{
    struct efs_msg_io_stats_reply r;
    uint64_t now = efs_iostats_now_us();
    int any = 0;
    int i;

    if (!force && now - g_last_dump_us < 5000000ull)
        return;
    efs_iostats_snapshot(&r);
    for (i = 0; i < EFS_IO_STATS_CLASSES; i++)
        if (r.cls[i].ops)
            any = 1;
    if (!any && !force)
        return;
    g_last_dump_us = now;
    fprintf(stderr,
            "iostats: get ops=%llu bytes=%llu errors=%llu avg_us=%llu "
            "p50_us=%llu max_us=%llu "
            "put ops=%llu bytes=%llu errors=%llu avg_us=%llu "
            "p50_us=%llu max_us=%llu "
            "wdisk ops=%llu bytes=%llu errors=%llu avg_us=%llu "
            "p50_us=%llu max_us=%llu\n",
            (unsigned long long)r.cls[EFS_IOSTAT_GET].ops,
            (unsigned long long)r.cls[EFS_IOSTAT_GET].bytes,
            (unsigned long long)r.cls[EFS_IOSTAT_GET].errors,
            (unsigned long long)(r.cls[EFS_IOSTAT_GET].ops
                                     ? r.cls[EFS_IOSTAT_GET].us_sum /
                                           r.cls[EFS_IOSTAT_GET].ops
                                     : 0),
            (unsigned long long)r.cls[EFS_IOSTAT_GET].p50_us,
            (unsigned long long)r.cls[EFS_IOSTAT_GET].us_max,
            (unsigned long long)r.cls[EFS_IOSTAT_PUT].ops,
            (unsigned long long)r.cls[EFS_IOSTAT_PUT].bytes,
            (unsigned long long)r.cls[EFS_IOSTAT_PUT].errors,
            (unsigned long long)(r.cls[EFS_IOSTAT_PUT].ops
                                     ? r.cls[EFS_IOSTAT_PUT].us_sum /
                                           r.cls[EFS_IOSTAT_PUT].ops
                                     : 0),
            (unsigned long long)r.cls[EFS_IOSTAT_PUT].p50_us,
            (unsigned long long)r.cls[EFS_IOSTAT_PUT].us_max,
            (unsigned long long)r.cls[EFS_IOSTAT_DISK_WRITE].ops,
            (unsigned long long)r.cls[EFS_IOSTAT_DISK_WRITE].bytes,
            (unsigned long long)r.cls[EFS_IOSTAT_DISK_WRITE].errors,
            (unsigned long long)(r.cls[EFS_IOSTAT_DISK_WRITE].ops
                                     ? r.cls[EFS_IOSTAT_DISK_WRITE].us_sum /
                                           r.cls[EFS_IOSTAT_DISK_WRITE].ops
                                     : 0),
            (unsigned long long)r.cls[EFS_IOSTAT_DISK_WRITE].p50_us,
            (unsigned long long)r.cls[EFS_IOSTAT_DISK_WRITE].us_max);
}
