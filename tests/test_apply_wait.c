/* W159: the applied-wait is bounded by progress (src/server/apply_wait.h).
 * Pins: one grant expires into BUSY without progress; an advancing applied
 * index renews the grant from the new index, as often as it advances; the
 * cap ends the wait whatever the progress; a grant never reaches past the
 * cap; remaining time is what the grant has left. Build: make
 * tests/test_apply_wait; run: ./tests/test_apply_wait (part of make test). */
#include <stdio.h>
#include <stdlib.h>
#include "../src/server/apply_wait.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    struct efs_apply_wait w;
    const uint64_t budget = 400000, cap = 8000000;

    /* No progress: BUSY once the grant is over, not before. */
    efs_apply_wait_start(&w, 1000, 10, budget, cap);
    CHECK(!efs_apply_wait_expired(&w, 1000, 10));
    CHECK(!efs_apply_wait_expired(&w, 1000 + budget - 1, 10));
    CHECK(efs_apply_wait_remaining_us(&w, 1000 + budget - 1) == 1);
    CHECK(efs_apply_wait_expired(&w, 1000 + budget, 10));
    CHECK(w.extensions == 0);

    /* Progress since the grant was set: renewed from the new index. */
    efs_apply_wait_start(&w, 0, 10, budget, cap);
    CHECK(!efs_apply_wait_expired(&w, budget, 11));
    CHECK(w.extensions == 1 && w.base == 11 && w.end_us == 2 * budget);
    CHECK(efs_apply_wait_remaining_us(&w, budget + 1) == budget - 1);
    /* The same index again at the next expiry: stalled, BUSY. */
    CHECK(efs_apply_wait_expired(&w, 2 * budget, 11));
    /* A lower index (replica replaced) is not progress either. */
    efs_apply_wait_start(&w, 0, 10, budget, cap);
    CHECK(efs_apply_wait_expired(&w, budget, 9));

    /* Progress every grant: renewed up to the cap, then BUSY at the cap. */
    efs_apply_wait_start(&w, 0, 0, budget, cap);
    uint64_t now = 0, applied = 0;
    unsigned grants = 0;
    while (!efs_apply_wait_expired(&w, now, applied)) {
        now = w.end_us;
        applied++;
        grants++;
        CHECK(w.end_us <= cap);
        CHECK(grants < 100);
    }
    CHECK(now == cap);
    CHECK(w.extensions == 19);
    CHECK(efs_apply_wait_remaining_us(&w, now) == 0);

    /* A cap shorter than one grant bounds the first grant too. */
    efs_apply_wait_start(&w, 5, 0, budget, 100);
    CHECK(w.end_us == 105);
    CHECK(!efs_apply_wait_expired(&w, 104, 1));
    CHECK(efs_apply_wait_expired(&w, 105, 1));

    if (fails) {
        fprintf(stderr, "test_apply_wait: %d FAIL\n", fails);
        return 1;
    }
    printf("test_apply_wait: PASS\n");
    return 0;
}
