#ifndef EFS_WRITE_PROF_H
#define EFS_WRITE_PROF_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* EFS_WRITE_PROF=1: where the leader's write path spends its time, per
 * phase, as log2 histograms so p50/p95/p99 are available (each quantile is
 * reported as its bucket's upper bound: within 2x, never below the truth).
 * Off by default; when off the hot path pays one predictable branch.
 *
 * Phases overlap and are not additive: a proposer's persist overlaps other
 * proposers' appends, and commit_apply starts when propose returned, which
 * for a held submit is after its local fsync. Each phase is the wall time
 * one thread spent in that step. */
enum {
    WP_HOLD,         /* efs_raft_disk_sync_hold in propose */
    WP_MU,           /* h->mu acquisition, first lock in propose */
    WP_APPEND,       /* h->mu held: local append + submit (or solo propose) */
    WP_SPIN,         /* sched_yield batching before the hold's fsync */
    WP_PERSIST,      /* hold release: local fsync of the burst */
    WP_MU_AFTER,     /* h->mu re-acquisition after the fsync */
    WP_PROPOSE,      /* host_propose_impl total, local leader */
    WP_COMMIT_APPLY, /* propose return -> entry applied (quorum + pump + apply) */
    WP_WAKE,         /* entry applied -> waiter has the verdict */
    WP_VERDICT,      /* reading the verdict once applied (incl. any lock) */
    WP_APPLY,        /* host_apply of one entry, in the pump */
    WP_COVER,        /* host_leader_cover_pick */
    WP_REPORT_MU,    /* h->mu acquisition in host_report_release */
    WP_N
};

static const char *const efs_wprof_names[WP_N] = {
    "hold", "mu", "append", "spin", "persist", "mu_after", "propose",
    "commit_apply", "wake", "verdict", "apply", "cover", "report_mu",
};

#define WPROF_BUCKETS 32
struct efs_wprof_hist {
    uint64_t n, sum, max;
    uint64_t b[WPROF_BUCKETS];
};

/* Bucket 0 holds 0 us; bucket k (k >= 1) holds [2^(k-1), 2^k) us. */
static inline unsigned efs_wprof_bucket(uint64_t us)
{
    unsigned k = us ? 64u - (unsigned)__builtin_clzll(us) : 0u;
    return k < WPROF_BUCKETS ? k : WPROF_BUCKETS - 1;
}

static inline void efs_wprof_add(struct efs_wprof_hist *h, uint64_t us)
{
    uint64_t m;

    __atomic_fetch_add(&h->n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&h->sum, us, __ATOMIC_RELAXED);
    __atomic_fetch_add(&h->b[efs_wprof_bucket(us)], 1, __ATOMIC_RELAXED);
    m = __atomic_load_n(&h->max, __ATOMIC_RELAXED);
    while (us > m &&
           !__atomic_compare_exchange_n(&h->max, &m, us, 1, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        ;
}

/* Upper bound (us) of the bucket holding the permille-th sample. */
static inline uint64_t efs_wprof_quantile(const uint64_t b[WPROF_BUCKETS],
                                          uint64_t n, unsigned permille)
{
    uint64_t want, seen = 0;

    if (!n)
        return 0;
    want = (n * permille + 999) / 1000;
    if (!want)
        want = 1;
    for (unsigned k = 0; k < WPROF_BUCKETS; k++) {
        seen += b[k];
        if (seen >= want)
            return k ? (1ull << k) - 1 : 0;
    }
    return UINT64_MAX;
}

/* One line: " name=n/sum_us/max_us/p50/p95/p99" per phase with samples.
 * Counters are cumulative since start; diff two lines for an interval. */
static inline int efs_wprof_format(char *buf, size_t cap,
                                   const struct efs_wprof_hist *hs,
                                   uint64_t mono_us, uint64_t arc_fallback)
{
    int n = snprintf(buf, cap, "write-phase-prof mono_us=%llu arc_fallback=%llu",
                     (unsigned long long)mono_us,
                     (unsigned long long)arc_fallback);

    if (n < 0 || (size_t)n >= cap)
        return -1;
    for (unsigned p = 0; p < WP_N; p++) {
        uint64_t b[WPROF_BUCKETS], cnt = 0;
        size_t used = (size_t)n;
        int w;

        for (unsigned k = 0; k < WPROF_BUCKETS; k++) {
            b[k] = __atomic_load_n(&hs[p].b[k], __ATOMIC_RELAXED);
            cnt += b[k];
        }
        if (!cnt)
            continue;
        w = snprintf(buf + used, cap - used, " %s=%llu/%llu/%llu/%llu/%llu/%llu",
                     efs_wprof_names[p], (unsigned long long)cnt,
                     (unsigned long long)__atomic_load_n(&hs[p].sum, __ATOMIC_RELAXED),
                     (unsigned long long)__atomic_load_n(&hs[p].max, __ATOMIC_RELAXED),
                     (unsigned long long)efs_wprof_quantile(b, cnt, 500),
                     (unsigned long long)efs_wprof_quantile(b, cnt, 950),
                     (unsigned long long)efs_wprof_quantile(b, cnt, 990));
        if (w < 0 || (size_t)w >= cap - used)
            return -1;
        n += w;
    }
    return n;
}
#endif
