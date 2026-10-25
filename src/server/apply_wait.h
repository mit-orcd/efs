#ifndef EFS_APPLY_WAIT_H
#define EFS_APPLY_WAIT_H
#include <stdint.h>

/* W159: a handler that waits for the pump to apply an index (its own
 * committed entry in host_wait_verdict, or a ReadIndex it must serve
 * behind) used one fixed budget, HOST_READ_TRIES x HOST_TICK_US = 400 ms,
 * and answered BUSY when the applied index had not reached it by then. On
 * a member whose durable barriers are slow (four members on one consumer
 * NVMe in the expiry fixture: 7-28 ms per fdatasync, queued on one
 * device), the pump keeps applying but its lag passes 400 ms; every BUSY
 * then sends the client down its 50..800 ms backoff ladder and the request
 * is proposed again behind the same lag. A 0.4 s wait became the 2-7 s
 * REPORT and CREATE RPCs of the intel1 run (seven attempts after BUSY).
 *
 * The wait is now bounded by progress, not by one clock: when the budget
 * expires and the progress value has advanced since the budget was set,
 * the waiter gets the same budget again from the new value. The value is
 * monotonic: raft_host.c passes the group's published applied index plus
 * the member's count of applies so far, so a pump cycle that applies for
 * longer than one grant (it publishes its view only at the end) still
 * counts as progress while every apply in it moves the count. A pump that
 * made no progress for a whole budget still answers BUSY (a stuck apply, a
 * lost replica), and the whole wait never exceeds cap_us whatever the
 * progress. Pure arithmetic in microseconds so tests/test_apply_wait.c
 * can pin it. */
struct efs_apply_wait {
    uint64_t budget_us;   /* one grant */
    uint64_t end_us;      /* current grant's expiry */
    uint64_t cap_us;      /* absolute end of the whole wait */
    uint64_t base;        /* progress value the current grant was set from */
    unsigned extensions;  /* grants after the first */
};

static inline void efs_apply_wait_start(struct efs_apply_wait *w, uint64_t now_us,
                                        uint64_t applied, uint64_t budget_us,
                                        uint64_t cap_us)
{
    w->budget_us = budget_us;
    w->cap_us = now_us + cap_us;
    w->end_us = now_us + budget_us;
    if (w->end_us > w->cap_us)
        w->end_us = w->cap_us;
    w->base = applied;
    w->extensions = 0;
}

/* 0: keep waiting (the grant may have been renewed); 1: give up (BUSY). */
static inline int efs_apply_wait_expired(struct efs_apply_wait *w, uint64_t now_us,
                                         uint64_t applied)
{
    if (now_us < w->end_us)
        return 0;
    if (applied <= w->base || now_us >= w->cap_us)
        return 1;
    w->base = applied;
    w->end_us = now_us + w->budget_us;
    if (w->end_us > w->cap_us)
        w->end_us = w->cap_us;
    w->extensions++;
    return 0;
}

/* Microseconds left in the current grant (0 when it has expired). */
static inline uint64_t efs_apply_wait_remaining_us(const struct efs_apply_wait *w,
                                                   uint64_t now_us)
{
    return now_us < w->end_us ? w->end_us - now_us : 0;
}
#endif
