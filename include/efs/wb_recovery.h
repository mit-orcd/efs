#ifndef EFS_WB_RECOVERY_H
#define EFS_WB_RECOVERY_H

#include <stdint.h>
#include <string.h>

/* D27 policy primitives. Caller serializes access. A record enters/leaves
 * unresolved exactly once; observing an error never retires dirty data.
 * Open descriptions sample sequence at open and retain their own cursor. */
struct efs_wb_error_state {
    uint64_t sequence;
    uint64_t unresolved;
};

static inline void efs_wb_error_add(struct efs_wb_error_state *s)
{
    ++s->sequence;
    ++s->unresolved;
}

/* Call only after that record's publication succeeds. */
static inline int efs_wb_error_resolve(struct efs_wb_error_state *s)
{
    if (!s->unresolved)
        return 0;
    --s->unresolved;
    return 1;
}

/* Nonzero means EIO. Successful recovery in this sync samples the sequence;
 * other descriptions still receive any error they have never observed. */
static inline int efs_wb_error_sync(const struct efs_wb_error_state *s,
                                   uint64_t *cursor, int recovered_here)
{
    int error = s->unresolved || (!recovered_here && *cursor != s->sequence);
    *cursor = s->sequence;
    return error;
}

struct efs_wb_cycle_state {
    uint64_t operation; /* stable application mutation identity, not PUT seq */
    uint64_t generation;
    uint64_t epoch;
    uint32_t spans;
    uint32_t unchanged;
    int valid;
};

/* Invoke once per COMPLETED fetch/rebase/publication attempt. Do not invoke
 * for failed fetches or standalone STALE replies. Returns a stall at eight
 * completed cycles with the same observed state and application operation. */
static inline int efs_wb_cycle_complete(struct efs_wb_cycle_state *s,
                                       uint64_t operation, uint64_t generation,
                                       uint64_t epoch, uint32_t spans)
{
    if (s->valid && s->operation == operation &&
        s->generation == generation && s->epoch == epoch && s->spans == spans) {
        if (s->unchanged < 8)
            ++s->unchanged;
    } else {
        s->operation = operation;
        s->generation = generation;
        s->epoch = epoch;
        s->spans = spans;
        s->unchanged = 1;
        s->valid = 1;
    }
    return s->unchanged >= 8;
}

static inline void efs_wb_cycle_reset(struct efs_wb_cycle_state *s)
{
    memset(s, 0, sizeof(*s));
}
#endif
