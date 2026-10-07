#include "../src/bench/latency.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
int main(void)
{
    struct lat_vec a = {0}, b = {0};
    uint64_t lo;
    assert(lat_percentile(&a, 99, &lo) == 0 && lo == 0);
    for (unsigned i = 1; i <= 100; i++) lat_add(&a, i);
    assert(lat_percentile(&a, 50, &lo) == 50 && lo == 50);
    assert(lat_percentile(&a, 99, &lo) == 99 && lo == 99);
    memset(&a, 0, sizeof(a));
    uint64_t values[10000], seed = 17;
    for (unsigned i = 0; i < 10000; i++) {
        seed = seed * UINT64_C(6364136223846793005) + 1;
        values[i] = (seed >> 20) % 10000000;
        lat_add(i % 2 ? &a : &b, values[i]);
    }
    lat_merge(&a, &b);
    qsort(values, 10000, sizeof(values[0]), cmp);
    for (unsigned pct = 1; pct <= 100; pct++) {
        uint64_t hi = lat_percentile(&a, pct, &lo), expected = values[pct * 100 - 1];
        assert(lo <= expected && expected <= hi);
        assert(hi - lo <= lo / 128);
    }
    assert(a.n == 10000 && a.max == values[9999]);
    memset(&a, 0, sizeof(a));
    /* Tail observations after the old million-sample cap must remain visible. */
    for (unsigned i = 0; i < 1100000; i++) lat_add(&a, 1);
    for (unsigned i = 0; i < 20000; i++) lat_add(&a, 1000000);
    assert(a.n == 1120000 && a.max == 1000000);
    assert(lat_percentile(&a, 99, &lo) == 1000000 && lo <= 1000000);
    memset(&a, 0, sizeof(a));
    lat_add(&a, UINT64_MAX);
    assert(lat_percentile(&a, 99, &lo) == UINT64_MAX && a.max == UINT64_MAX);
    puts("benchmark latency: full coverage, percentile bounds, merge and exact max PASS");
    return 0;
}
