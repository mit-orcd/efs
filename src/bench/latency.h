#ifndef EFS_BENCH_LATENCY_H
#define EFS_BENCH_LATENCY_H
#include <stdint.h>
#include <string.h>

/* Every observation is counted; no allocation or sample cap. Exact integer
 * microseconds through 255, then power-of-two ranges with 128 subdivisions.
 * Non-exact buckets have relative width below 1/128 (0.782%). Percentiles are
 * reported as intervals, with the existing pXX_us field holding the upper end.
 * Maximum is tracked independently and is never quantized. */
#define BENCH_LAT_BUCKETS (58u * 128u)
struct lat_vec {
    uint64_t n, max;
    uint64_t bins[BENCH_LAT_BUCKETS];
};
static inline unsigned lat_bucket(uint64_t us)
{
    if (us < 256) return (unsigned)us;
    unsigned shift = 63u - (unsigned)__builtin_clzll(us) - 7u;
    return shift * 128u + (unsigned)(us >> shift);
}
static inline void lat_add(struct lat_vec *v, uint64_t us)
{
    v->bins[lat_bucket(us)]++;
    v->n++;
    if (us > v->max) v->max = us;
}
static inline void lat_merge(struct lat_vec *dst, const struct lat_vec *src)
{
    for (unsigned i = 0; i < BENCH_LAT_BUCKETS; i++) dst->bins[i] += src->bins[i];
    dst->n += src->n;
    if (src->max > dst->max) dst->max = src->max;
}
static inline uint64_t lat_percentile(const struct lat_vec *v, unsigned pct, uint64_t *low)
{
    if (!v->n) { if (low) *low = 0; return 0; }
    /* Nearest rank, without multiplying the entire count. */
    uint64_t rank = (v->n / 100) * pct + ((v->n % 100) * pct + 99) / 100;
    if (!rank) rank = 1;
    uint64_t count = 0;
    for (unsigned i = 0; i < BENCH_LAT_BUCKETS; i++) {
        count += v->bins[i];
        if (count < rank) continue;
        unsigned shift = i < 256 ? 0 : i / 128 - 1;
        uint64_t lo = i < 256 ? i : (uint64_t)(128 + i % 128) << shift;
        uint64_t hi = lo + ((UINT64_C(1) << shift) - 1);
        if (hi > v->max) hi = v->max;
        if (low) *low = lo;
        return hi;
    }
    if (low) *low = v->max;
    return v->max;
}
#endif
